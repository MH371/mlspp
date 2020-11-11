#include <mls/state.h>

namespace mls {

///
/// Constructors
///

State::State(bytes group_id,
             CipherSuite suite,
             const HPKEPrivateKey& init_priv,
             SignaturePrivateKey sig_priv,
             const KeyPackage& key_package)
  : _suite(suite)
  , _group_id(std::move(group_id))
  , _epoch(0)
  , _tree(suite)
  , _keys(suite)
  , _index(0)
  , _identity_priv(std::move(sig_priv))
{
  auto index = _tree.add_leaf(key_package);
  _tree.set_hash_all();
  _tree_priv = TreeKEMPrivateKey::solo(suite, index, init_priv);
}

// Initialize a group from a Welcome
State::State(const HPKEPrivateKey& init_priv,
             SignaturePrivateKey sig_priv,
             const KeyPackage& kp,
             const Welcome& welcome)
  : _suite(welcome.cipher_suite)
  , _tree(welcome.cipher_suite)
  , _identity_priv(std::move(sig_priv))
{
  auto maybe_kpi = welcome.find(kp);
  if (!maybe_kpi.has_value()) {
    throw InvalidParameterError("Welcome not intended for key package");
  }
  auto kpi = maybe_kpi.value();

  if (kp.cipher_suite != welcome.cipher_suite) {
    throw InvalidParameterError("Ciphersuite mismatch");
  }

  // Decrypt the GroupSecrets
  auto secrets_ct = welcome.secrets[kpi].encrypted_group_secrets;
  auto secrets_data = init_priv.decrypt(kp.cipher_suite, {}, secrets_ct);
  auto secrets = tls::get<GroupSecrets>(secrets_data);

  // Decrypt the GroupInfo and fill in details
  auto group_info = welcome.decrypt(secrets.joiner_secret, {});
  group_info.tree.suite = kp.cipher_suite;
  group_info.tree.set_hash_all();

  // Verify the signature on the GroupInfo
  if (!group_info.verify()) {
    throw InvalidParameterError("Invalid GroupInfo");
  }

  // Verify the incoming tree
  if (!group_info.tree.parent_hash_valid()) {
    throw InvalidParameterError("Invalid tree");
  }

  // Ingest the GroupSecrets and GroupInfo
  _epoch = group_info.epoch;
  _group_id = group_info.group_id;
  _tree = group_info.tree;
  _confirmed_transcript_hash = group_info.confirmed_transcript_hash;
  _interim_transcript_hash = group_info.interim_transcript_hash;

  // Construct TreeKEM private key from partrs provided
  auto maybe_index = _tree.find(kp);
  if (!maybe_index.has_value()) {
    throw InvalidParameterError("New joiner not in tree");
  }

  _index = maybe_index.value();

  auto ancestor = tree_math::ancestor(_index, group_info.signer_index);
  auto path_secret = std::optional<bytes>{};
  if (secrets.path_secret.has_value()) {
    path_secret = secrets.path_secret.value().secret;
  }

  _tree_priv = TreeKEMPrivateKey::joiner(
    _suite, _tree.size(), _index, init_priv, ancestor, path_secret);

  // Ratchet forward into the current epoch
  auto group_ctx = tls::marshal(group_context());
  _keys = KeyScheduleEpoch(
    _suite, secrets.joiner_secret, {}, group_ctx, LeafCount(_tree.size()));

  // Verify the confirmation
  if (!verify_confirmation(group_info.confirmation)) {
    throw ProtocolError("Confirmation failed to verify");
  }
}

///
/// Proposal and commit factories
///

MLSPlaintext
State::sign(const Proposal& proposal) const
{
  auto sender = Sender{ SenderType::member, _index.val };
  auto pt = MLSPlaintext{ _group_id, _epoch, sender, proposal };
  pt.sign(_suite, group_context(), _identity_priv);
  pt.set_membership_tag(_suite, group_context(), _keys.membership_key);
  return pt;
}

Proposal
State::add_proposal(const KeyPackage& key_package) const
{
  // Check that the key package is validly signed
  if (!key_package.verify()) {
    throw InvalidParameterError("Invalid signature on key package");
  }

  // Check that the group's basic properties are supported
  auto now = seconds_since_epoch();
  if (!key_package.verify_expiry(now)) {
    throw InvalidParameterError("Expired key package");
  }

  // Check that the group's extensions are supported
  if (!key_package.verify_extension_support(_extensions)) {
    throw InvalidParameterError(
      "Key package does not support group's extensions");
  }

  return { Add{ key_package } };
}

Proposal
State::update_proposal(const bytes& leaf_secret)
{
  // TODO(RLB) Allow changing the signing key
  auto kp = _tree.key_package(_index).value();
  kp.init_key = HPKEPrivateKey::derive(_suite, leaf_secret).public_key;
  kp.sign(_identity_priv, std::nullopt);

  _update_secrets[kp.hash()] = leaf_secret;
  return { Update{ kp } };
}

Proposal
State::remove_proposal(RosterIndex index) const
{
  return remove_proposal(leaf_for_roster_entry(index));
}

Proposal
State::remove_proposal(LeafIndex removed) const
{
  return { Remove{ removed } };
}

MLSPlaintext
State::add(const KeyPackage& key_package) const
{
  return sign(add_proposal(key_package));
}

MLSPlaintext
State::update(const bytes& leaf_secret)
{
  return sign(update_proposal(leaf_secret));
}

MLSPlaintext
State::remove(RosterIndex index) const
{
  return sign(remove_proposal(index));
}

MLSPlaintext
State::remove(LeafIndex removed) const
{
  return sign(remove_proposal(removed));
}

std::tuple<MLSPlaintext, Welcome, State>
State::commit(const bytes& leaf_secret,
              const std::vector<Proposal>& extra_proposals) const
{
  // Construct a commit from cached proposals
  // TODO(rlb) ignore some proposals:
  // * Update after Update
  // * Update after Remove
  // * Remove after Remove
  Commit commit;
  auto joiners = std::vector<KeyPackage>{};
  for (const auto& cached : _pending_proposals) {
    if (std::holds_alternative<Add>(cached.proposal.content)) {
      const auto& add = std::get<Add>(cached.proposal.content);
      joiners.push_back(add.key_package);
    }

    commit.proposals.push_back({ ProposalRef{ cached.ref } });
  }

  // Add the extra proposals to those we had cached
  for (const auto& proposal : extra_proposals) {
    if (std::holds_alternative<Add>(proposal.content)) {
      const auto& add = std::get<Add>(proposal.content);
      joiners.push_back(add.key_package);
    }

    commit.proposals.push_back({ proposal });
  }

  // Apply proposals
  State next = successor();
  auto proposals = must_resolve(commit.proposals, _index);
  auto [has_updates, has_removes, joiner_locations] = next.apply(proposals);

  // KEM new entropy to the group and the new joiners
  auto path_required = has_updates || has_removes || commit.proposals.empty();
  auto update_secret = bytes(_suite.get().hpke.kdf.hash_size(), 0);
  auto path_secrets =
    std::vector<std::optional<bytes>>(joiner_locations.size());
  if (path_required) {
    auto ctx = tls::marshal(GroupContext{
      next._group_id,
      next._epoch + 1,
      next._tree.root_hash(),
      next._confirmed_transcript_hash,
      next._extensions,
    });
    auto [new_priv, path] = next._tree.encap(
      next._index, ctx, leaf_secret, _identity_priv, std::nullopt);
    next._tree_priv = new_priv;
    commit.path = path;
    update_secret = new_priv.update_secret;

    for (size_t i = 0; i < joiner_locations.size(); i++) {
      auto [overlap, shared_path_secret, ok] =
        new_priv.shared_path_secret(joiner_locations[i]);
      silence_unused(overlap);
      silence_unused(ok);

      path_secrets[i] = shared_path_secret;
    }
  }

  // Create the Commit message and advance the transcripts / key schedule
  auto pt = next.ratchet_and_sign(commit, update_secret, group_context());

  // Complete the GroupInfo and form the Welcome
  auto group_info = GroupInfo{
    next._group_id,
    next._epoch,
    next._tree,
    next._confirmed_transcript_hash,
    next._interim_transcript_hash,
    next._extensions,
    pt.confirmation_tag.value().mac_value,
  };
  group_info.sign(next._index, _identity_priv);

  auto welcome = Welcome{ _suite, next._keys.joiner_secret, {}, group_info };
  for (size_t i = 0; i < joiners.size(); i++) {
    welcome.encrypt(joiners[i], path_secrets[i]);
  }

  return std::make_tuple(pt, welcome, next);
}

///
/// Message handlers
///

GroupContext
State::group_context() const
{
  return GroupContext{
    _group_id,   _epoch, _tree.root_hash(), _confirmed_transcript_hash,
    _extensions,
  };
}

MLSPlaintext
State::ratchet_and_sign(const Commit& op,
                        const bytes& update_secret,
                        const GroupContext& prev_ctx)
{
  auto prev_membership_key = _keys.membership_key;
  auto sender = Sender{ SenderType::member, _index.val };
  auto pt = MLSPlaintext{ _group_id, _epoch, sender, op };
  pt.sign(_suite, prev_ctx, _identity_priv);

  auto confirmed_transcript = _interim_transcript_hash + pt.commit_content();
  _confirmed_transcript_hash = _suite.get().digest.hash(confirmed_transcript);
  _epoch += 1;
  update_epoch_secrets(update_secret);

  auto confirmation = _suite.get().digest.hmac(_keys.confirmation_key,
                                               _confirmed_transcript_hash);
  pt.confirmation_tag = { std::move(confirmation) };
  pt.set_membership_tag(_suite, prev_ctx, prev_membership_key);

  auto interim_transcript = _confirmed_transcript_hash + pt.commit_auth_data();
  _interim_transcript_hash = _suite.get().digest.hash(interim_transcript);

  return pt;
}

std::optional<State>
State::handle(const MLSPlaintext& pt)
{
  // Pre-validate the MLSPlaintext
  if (pt.group_id != _group_id) {
    throw InvalidParameterError("GroupID mismatch");
  }

  if (pt.epoch != _epoch) {
    throw InvalidParameterError("Epoch mismatch");
  }

  if (!verify(pt)) {
    throw ProtocolError("Invalid handshake message signature");
  }

  // Proposals get queued, do not result in a state transition
  if (std::holds_alternative<Proposal>(pt.content)) {
    cache_proposal(pt);
    return std::nullopt;
  }

  if (!std::holds_alternative<Commit>(pt.content)) {
    throw InvalidParameterError("Incorrect content type");
  }

  if (pt.sender.sender_type != SenderType::member) {
    throw ProtocolError("Commit must originate from within the group");
  }

  auto sender = LeafIndex(pt.sender.sender);
  if (sender == _index) {
    throw InvalidParameterError("Handle own commits with caching");
  }

  // Apply the commit
  const auto& commit = std::get<Commit>(pt.content);
  const auto proposals = must_resolve(commit.proposals, sender);

  auto next = successor();
  next.apply(proposals);

  // Decapsulate and apply the UpdatePath, if provided
  // TODO(RLB) Verify that path is provided if required
  auto update_secret = bytes(_suite.get().hpke.kdf.hash_size(), 0);
  if (commit.path.has_value()) {
    auto ctx = tls::marshal(GroupContext{
      next._group_id,
      next._epoch + 1,
      next._tree.root_hash(),
      next._confirmed_transcript_hash,
      next._extensions,
    });
    const auto& path = commit.path.value();
    next._tree_priv.decap(sender, next._tree, ctx, path);
    next._tree.merge(sender, path);
    update_secret = next._tree_priv.update_secret;
  }

  // Update the transcripts and advance the key schedule
  next._confirmed_transcript_hash = _suite.get().digest.hash(
    next._interim_transcript_hash + pt.commit_content());
  next._interim_transcript_hash = _suite.get().digest.hash(
    next._confirmed_transcript_hash + pt.commit_auth_data());

  next._epoch += 1;
  next.update_epoch_secrets(update_secret);

  // Verify the confirmation MAC
  if (!pt.confirmation_tag.has_value()) {
    throw ProtocolError("Missing confirmation on Commit");
  }

  if (!next.verify_confirmation(pt.confirmation_tag.value().mac_value)) {
    throw ProtocolError("Confirmation failed to verify");
  }

  return next;
}

LeafIndex
State::apply(const Add& add)
{
  return _tree.add_leaf(add.key_package);
}

void
State::apply(LeafIndex target, const Update& update)
{
  _tree.update_leaf(target, update.key_package);
}

void
State::apply(LeafIndex target, const Update& update, const bytes& leaf_secret)
{
  _tree.update_leaf(target, update.key_package);
  _tree_priv.set_leaf_secret(leaf_secret);
}

void
State::apply(const Remove& remove)
{
  _tree.blank_path(remove.removed);
}

void
State::cache_proposal(const MLSPlaintext& pt)
{
  _pending_proposals.push_back({
    _suite.get().digest.hash(tls::marshal(pt)),
    std::get<Proposal>(pt.content),
    LeafIndex{ pt.sender.sender },
  });
}

std::optional<State::CachedProposal>
State::resolve(const ProposalID& id, LeafIndex sender_index) const
{
  if (std::holds_alternative<Proposal>(id.content)) {
    return CachedProposal{
      {},
      std::get<Proposal>(id.content),
      sender_index,
    };
  }

  const auto& ref = std::get<ProposalRef>(id.content);
  for (const auto& cached : _pending_proposals) {
    if (cached.ref == ref.id) {
      return cached;
    }
  }

  return std::nullopt;
}

std::vector<State::CachedProposal>
State::must_resolve(const std::vector<ProposalID>& ids,
                    LeafIndex sender_index) const
{
  auto proposals = std::vector<CachedProposal>(ids.size());
  auto must_resolve = [&](auto& id) {
    return resolve(id, sender_index).value();
  };
  std::transform(ids.begin(), ids.end(), proposals.begin(), must_resolve);
  return proposals;
}

std::vector<LeafIndex>
State::apply(const std::vector<CachedProposal>& proposals,
             ProposalType::selector required_type)
{
  auto locations = std::vector<LeafIndex>{};
  for (const auto& cached : proposals) {
    auto proposal_type = cached.proposal.proposal_type();
    if (proposal_type != required_type) {
      continue;
    }

    switch (proposal_type) {
      case ProposalType::selector::add: {
        locations.push_back(apply(std::get<Add>(cached.proposal.content)));
        break;
      }

      case ProposalType::selector::update: {
        auto& update = std::get<Update>(cached.proposal.content);
        if (cached.sender != _index) {
          apply(cached.sender, update);
          break;
        }

        auto kp_hash = update.key_package.hash();
        if (_update_secrets.count(kp_hash) == 0) {
          throw ProtocolError("Self-update with no cached secret");
        }

        apply(cached.sender, update, _update_secrets[cached.ref]);
        locations.push_back(cached.sender);
        break;
      }

      case ProposalType::selector::remove: {
        const auto& remove = std::get<Remove>(cached.proposal.content);
        apply(remove);
        locations.push_back(remove.removed);
        break;
      }

      default:
        throw ProtocolError("Unknown proposal type");
    }
  }

  return locations;
}

std::tuple<bool, bool, std::vector<LeafIndex>>
State::apply(const std::vector<CachedProposal>& proposals)
{
  auto update_locations = apply(proposals, ProposalType::selector::update);
  auto remove_locations = apply(proposals, ProposalType::selector::remove);
  auto joiner_locations = apply(proposals, ProposalType::selector::add);

  auto has_updates = !update_locations.empty();
  auto has_removes = !remove_locations.empty();

  _tree.truncate();
  _tree_priv.truncate(_tree.size());
  _tree.set_hash_all();
  return std::make_tuple(has_updates, has_removes, joiner_locations);
}

///
/// Message protection
///

MLSCiphertext
State::protect(const bytes& pt)
{
  auto sender = Sender{ SenderType::member, _index.val };
  MLSPlaintext mpt{ _group_id, _epoch, sender, ApplicationData{ pt } };
  mpt.sign(_suite, group_context(), _identity_priv);
  mpt.set_membership_tag(_suite, group_context(), _keys.membership_key);
  return encrypt(mpt);
}

bytes
State::unprotect(const MLSCiphertext& ct)
{
  MLSPlaintext pt = decrypt(ct);

  if (!verify(pt)) {
    throw ProtocolError("Invalid message signature");
  }

  if (!std::holds_alternative<ApplicationData>(pt.content)) {
    throw ProtocolError("Unprotect of non-application message");
  }

  // NOLINTNEXTLINE(cppcoreguidelines-slicing)
  return std::get<ApplicationData>(pt.content).data;
}

///
/// Inner logic and convenience functions
///

bool
operator==(const State& lhs, const State& rhs)
{
  auto suite = (lhs._suite == rhs._suite);
  auto group_id = (lhs._group_id == rhs._group_id);
  auto epoch = (lhs._epoch == rhs._epoch);
  auto tree = (lhs._tree == rhs._tree);
  auto confirmed_transcript_hash =
    (lhs._confirmed_transcript_hash == rhs._confirmed_transcript_hash);
  auto interim_transcript_hash =
    (lhs._interim_transcript_hash == rhs._interim_transcript_hash);
  auto keys = (lhs._keys == rhs._keys);

  return suite && group_id && epoch && tree && confirmed_transcript_hash &&
         interim_transcript_hash && keys;
}

bool
operator!=(const State& lhs, const State& rhs)
{
  return !(lhs == rhs);
}

void
State::update_epoch_secrets(const bytes& commit_secret)
{
  auto ctx = tls::marshal(GroupContext{
    _group_id,
    _epoch,
    _tree.root_hash(),
    _confirmed_transcript_hash,
    _extensions,
  });
  _keys = _keys.next(commit_secret, {}, ctx, LeafCount{ _tree.size() });
}

///
/// Message encryption and decryption
///

bool
State::verify_internal(const MLSPlaintext& pt) const
{
  if (!pt.verify_membership_tag(
        _suite, group_context(), _keys.membership_key)) {
    return false;
  }

  auto maybe_kp = _tree.key_package(LeafIndex(pt.sender.sender));
  if (!maybe_kp.has_value()) {
    throw InvalidParameterError("Signature from blank node");
  }

  const auto& pub = maybe_kp.value().credential.public_key();
  return pt.verify(_suite, group_context(), pub);
}

bool
State::verify(const MLSPlaintext& pt) const
{
  switch (pt.sender.sender_type) {
    case SenderType::member:
      return verify_internal(pt);

    default:
      // TODO(RLB) Support other sender types
      throw NotImplementedError();
  }
}

bool
State::verify_confirmation(const bytes& confirmation) const
{
  auto confirm = _suite.get().digest.hmac(_keys.confirmation_key,
                                          _confirmed_transcript_hash);
  return constant_time_eq(confirm, confirmation);
}

bytes
State::do_export(const std::string& label,
                 const bytes& context,
                 size_t size) const
{
  auto secret = _suite.derive_secret(_keys.exporter_secret, label);
  auto context_hash = _suite.get().digest.hash(context);
  return _suite.expand_with_label(secret, "exporter", context_hash, size);
}

std::vector<KeyPackage>
State::roster() const
{
  auto kps = std::vector<KeyPackage>(_tree.size().val);
  auto leaf_count = uint32_t(0);

  for (uint32_t i = 0; i < _tree.size().val; i++) {
    const auto& kp = _tree.key_package(LeafIndex{ i });
    if (!kp.has_value()) {
      continue;
    }
    kps.at(leaf_count) = kp.value();
    leaf_count++;
  }

  kps.resize(leaf_count);
  return kps;
}

bytes
State::authentication_secret() const
{
  return _keys.authentication_secret;
}

// struct {
//     opaque group_id<0..255>;
//     uint64 epoch;
//     ContentType content_type;
//     opaque authenticated_data<0..2^32-1>;
// } MLSCiphertextContentAAD;
struct MLSCiphertextContentAAD
{
  const bytes& group_id;
  const epoch_t epoch;
  const ContentType::selector content_type;
  const bytes& authenticated_data;

  TLS_SERIALIZABLE(group_id, epoch, content_type, authenticated_data)
  TLS_TRAITS(tls::vector<1>, tls::pass, tls::pass, tls::vector<4>)
};

using ReuseGuard = std::array<uint8_t, 4>;

static ReuseGuard
new_reuse_guard()
{
  auto random = random_bytes(4);
  auto guard = ReuseGuard();
  std::copy(random.begin(), random.end(), guard.begin());
  return guard;
}

static void
apply_reuse_guard(const ReuseGuard& guard, bytes& nonce)
{
  for (size_t i = 0; i < guard.size(); i++) {
    nonce.at(i) ^= guard.at(i);
  }
}

// struct {
//     uint32 sender;
//     uint32 generation;
//     opaque reuse_guard[4];
// } MLSSenderData;
struct MLSSenderData
{
  uint32_t sender;
  uint32_t generation;
  ReuseGuard reuse_guard;

  TLS_SERIALIZABLE(sender, generation, reuse_guard)
};

// struct {
//     opaque group_id<0..255>;
//     uint64 epoch;
//     ContentType content_type;
// } MLSSenderDataAAD;
struct MLSSenderDataAAD
{
  const bytes& group_id;
  const epoch_t epoch;
  const ContentType::selector content_type;

  TLS_SERIALIZABLE(group_id, epoch, content_type)
  TLS_TRAITS(tls::vector<1>, tls::pass, tls::pass)
};

MLSCiphertext
State::encrypt(const MLSPlaintext& pt)
{
  // Pull from the key schedule
  static const auto get_key_type = overloaded{
    [](const ApplicationData& /*unused*/) {
      return GroupKeySource::RatchetType::application;
    },
    [](const Proposal& /*unused*/) {
      return GroupKeySource::RatchetType::handshake;
    },
    [](const Commit& /*unused*/) {
      return GroupKeySource::RatchetType::handshake;
    },
  };

  auto key_type = std::visit(get_key_type, pt.content);
  auto [generation, keys] = _keys.keys.next(key_type, _index);

  // Encrypt the content
  // XXX(rlb@ipv.sx): Apply padding?
  auto content = pt.marshal_content(0);
  auto content_type = pt.content_type();
  auto content_aad = tls::marshal(MLSCiphertextContentAAD{
    _group_id, _epoch, content_type, pt.authenticated_data });

  auto reuse_guard = new_reuse_guard();
  apply_reuse_guard(reuse_guard, keys.nonce);

  auto content_ct =
    _suite.get().hpke.aead.seal(keys.key, keys.nonce, content_aad, content);

  // Encrypt the sender data
  auto sender_data_obj = MLSSenderData{ _index.val, generation, reuse_guard };
  auto sender_data = tls::marshal(sender_data_obj);

  auto [sender_data_key, sender_data_nonce] = _keys.sender_data(content_ct);
  auto sender_data_aad =
    tls::marshal(MLSSenderDataAAD{ _group_id, _epoch, content_type });

  auto encrypted_sender_data = _suite.get().hpke.aead.seal(
    sender_data_key, sender_data_nonce, sender_data_aad, sender_data);

  // Assemble the MLSCiphertext
  MLSCiphertext ct;
  ct.group_id = _group_id;
  ct.epoch = _epoch;
  ct.content_type = content_type;
  ct.encrypted_sender_data = encrypted_sender_data;
  ct.authenticated_data = pt.authenticated_data;
  ct.ciphertext = content_ct;
  return ct;
}

MLSPlaintext
State::decrypt(const MLSCiphertext& ct)
{
  // Verify the epoch
  if (ct.group_id != _group_id) {
    throw InvalidParameterError("Ciphertext not from this group");
  }

  if (ct.epoch != _epoch) {
    throw InvalidParameterError("Ciphertext not from this epoch");
  }

  // Decrypt and parse the sender data
  auto [sender_data_key, sender_data_nonce] = _keys.sender_data(ct.ciphertext);
  auto sender_data_aad =
    tls::marshal(MLSSenderDataAAD{ ct.group_id, ct.epoch, ct.content_type });
  auto sender_data_pt = _suite.get().hpke.aead.open(sender_data_key,
                                                    sender_data_nonce,
                                                    sender_data_aad,
                                                    ct.encrypted_sender_data);
  if (!sender_data_pt.has_value()) {
    throw ProtocolError("Sender data decryption failed");
  }

  auto sender_data = tls::get<MLSSenderData>(sender_data_pt.value());
  auto sender = LeafIndex(sender_data.sender);

  // Pull from the key schedule
  auto key_type = GroupKeySource::RatchetType::handshake;
  if (ct.content_type == ContentType::selector::application) {
    key_type = GroupKeySource::RatchetType::application;
  }

  auto [key, nonce] = _keys.keys.get(key_type, sender, sender_data.generation);
  _keys.keys.erase(key_type, sender, sender_data.generation);
  apply_reuse_guard(sender_data.reuse_guard, nonce);

  // Compute the plaintext AAD and decrypt
  auto content_aad = tls::marshal(MLSCiphertextContentAAD{
    ct.group_id,
    ct.epoch,
    ct.content_type,
    ct.authenticated_data,
  });
  auto content =
    _suite.get().hpke.aead.open(key, nonce, content_aad, ct.ciphertext);
  if (!content.has_value()) {
    throw ProtocolError("Content decryption failed");
  }

  // Set up a new plaintext based on the content
  return MLSPlaintext{ _group_id,
                       _epoch,
                       { SenderType::member, sender_data.sender },
                       ct.content_type,
                       ct.authenticated_data,
                       content.value() };
}

LeafIndex
State::leaf_for_roster_entry(RosterIndex index) const
{
  auto non_blank_leaves = uint32_t(0);

  for (auto i = LeafIndex{ 0 }; i < _tree.size(); i.val++) {
    const auto& kp = _tree.key_package(i);
    if (!kp.has_value()) {
      continue;
    }
    if (non_blank_leaves == index.val) {
      return i;
    }
    non_blank_leaves += 1;
  }

  throw InvalidParameterError("Leaf Index mismatch");
}

State
State::successor() const
{
  // Copy everything, then clear things that shouldn't be copied
  auto next = *this;
  next._pending_proposals.clear();
  return next;
}

} // namespace mls
