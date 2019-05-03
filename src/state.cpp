#include "state.h"

namespace mls {

///
/// GroupState
///

tls::ostream&
operator<<(tls::ostream& out, const GroupState& obj)
{
  return out << obj.group_id << obj.epoch << obj.tree_hash
             << obj.transcript_hash;
}

tls::istream&
operator>>(tls::istream& out, GroupState& obj)
{
  return out >> obj.group_id >> obj.epoch >> obj.tree_hash >>
         obj.transcript_hash;
}

///
/// ApplicationKeyChain
///

const char* KeyChain::_secret_label = "sender";
const char* KeyChain::_nonce_label = "nonce";
const char* KeyChain::_key_label = "key";

void
KeyChain::start(LeafIndex my_sender, const bytes& root_secret)
{
  _my_generation = 0;
  _my_sender = my_sender;
  _root_secret = root_secret;
}

KeyChain::Generation
KeyChain::next()
{
  _my_generation += 1;
  return get(_my_sender, _my_generation);
}

KeyChain::Generation
KeyChain::get(LeafIndex sender, uint32_t generation) const
{
  auto sender_bytes = tls::marshal(sender.val);
  auto secret = _root_secret;

  // Split off onto the sender chain
  secret = derive(secret, _secret_label, sender_bytes, _secret_size);

  // Work down the generations
  for (uint32_t i = 0; i < generation; ++i) {
    secret = derive(secret, _secret_label, sender_bytes, _secret_size);
  }

  // Derive the key and nonce
  auto key = derive(secret, _key_label, bytes(), _key_size);
  auto nonce = derive(secret, _nonce_label, bytes(), _nonce_size);

  return Generation{ generation, secret, key, nonce };
}

bytes
KeyChain::derive(const bytes& secret,
                 const std::string& label,
                 const bytes& context,
                 const size_t size) const
{
  return hkdf_expand_label(_suite, secret, label, context, size);
}

///
/// Constructors
///

State::State(bytes group_id,
             CipherSuite suite,
             const bytes& leaf_secret,
             SignaturePrivateKey identity_priv,
             const Credential& credential)
  : _suite(suite)
  , _group_id(std::move(group_id))
  , _epoch(0)
  , _tree(suite, leaf_secret, credential)
  , _transcript_hash(zero_bytes(Digest(suite).output_size()))
  , _init_secret(Digest(suite).output_size())
  , _handshake_keys(suite)
  , _application_keys(suite)
  , _index(0)
  , _identity_priv(std::move(identity_priv))
  , _zero(Digest(suite).output_size(), 0)
{}

State::State(SignaturePrivateKey identity_priv,
             const Credential& credential,
             const bytes& init_secret,
             const Welcome& welcome,
             const MLSPlaintext& handshake)
  : _suite(welcome.cipher_suite)
  , _tree(welcome.cipher_suite)
  , _handshake_keys(welcome.cipher_suite)
  , _application_keys(welcome.cipher_suite)
  , _identity_priv(std::move(identity_priv))
{
  // Verify that we have an add and it is for us
  if (handshake.operation.value().type != GroupOperationType::add) {
    throw InvalidParameterError("Incorrect handshake type");
  }

  auto operation = handshake.operation.value();
  auto add = operation.add.value();
  if (credential != add.init_key.credential) {
    throw InvalidParameterError("Add not targeted for this node");
  }

  // Make sure that the init key for the chosen ciphersuite is the
  // one we sent
  auto init_uik = add.init_key.find_init_key(_suite);
  if (!init_uik) {
    throw ProtocolError("Selected cipher suite not supported");
  }

  auto init_priv = DHPrivateKey::node_derive(_suite, init_secret);
  if (*init_uik != init_priv.public_key()) {
    throw ProtocolError("Incorrect init key");
  }

  // Decrypt the Welcome
  auto welcome_info = welcome.decrypt(init_priv);

  // Make sure the WelcomeInfo matches the Add
  if (add.welcome_info_hash != welcome_info.hash(_suite)) {
    throw ProtocolError("Mismatch in welcome info hash");
  }

  // Ingest the WelcomeInfo
  _epoch = welcome_info.epoch + 1;
  _group_id = welcome_info.group_id;
  _tree = welcome_info.tree;
  _transcript_hash = welcome_info.transcript_hash;

  _init_secret = welcome_info.init_secret;
  _zero = bytes(Digest(_suite).output_size(), 0);

  // Add to the transcript hash
  update_transcript_hash(operation);

  // Add to the tree
  _index = add.index;
  _tree.add_leaf(_index, init_secret, credential);

  // Ratchet forward into shared state
  update_epoch_secrets(_zero);

  if (!verify(handshake)) {
    throw InvalidParameterError("Handshake signature failed to verify");
  }
}

State::InitialInfo
State::negotiate(const bytes& group_id,
                 const std::vector<CipherSuite> supported_ciphersuites,
                 const bytes& leaf_secret,
                 const SignaturePrivateKey& identity_priv,
                 const Credential& credential,
                 const UserInitKey& user_init_key)
{
  // Negotiate a ciphersuite with the other party
  CipherSuite suite;
  auto selected = false;
  for (auto my_suite : supported_ciphersuites) {
    for (auto other_suite : user_init_key.cipher_suites) {
      if (my_suite == other_suite) {
        selected = true;
        suite = my_suite;
        break;
      }
    }

    if (selected) {
      break;
    }
  }

  if (!selected) {
    throw ProtocolError("Negotiation failure");
  }

  // We have manually guaranteed that `suite` is always initialized
  // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
  auto state = State{ group_id, suite, leaf_secret, identity_priv, credential };
  return state.add(user_init_key);
}

///
/// Message factories
///

std::tuple<Welcome, MLSPlaintext, State>
State::add(const UserInitKey& user_init_key) const
{
  return add(_tree.size(), user_init_key);
}

std::tuple<Welcome, MLSPlaintext, State>
State::add(uint32_t index, const UserInitKey& user_init_key) const
{
  if (!user_init_key.verify()) {
    throw InvalidParameterError("bad signature on user init key");
  }

  auto pub = user_init_key.find_init_key(_suite);
  if (!pub) {
    throw ProtocolError("New member does not support the group's ciphersuite");
  }

  auto welcome_info_str = welcome_info();
  auto welcome =
    Welcome{ user_init_key.user_init_key_id, *pub, welcome_info_str };
  auto welcome_tuple = std::make_tuple(welcome);

  auto welcome_info_hash = welcome_info_str.hash(_suite);
  auto add_state =
    sign(Add{ LeafIndex{ index }, user_init_key, welcome_info_hash });
  return std::tuple_cat(welcome_tuple, add_state);
}

std::tuple<MLSPlaintext, State>
State::update(const bytes& leaf_secret)
{
  auto path = _tree.encrypt(_index, leaf_secret);
  _cached_leaf_secret = leaf_secret;
  return sign(Update{ path });
}

std::tuple<MLSPlaintext, State>
State::remove(const bytes& evict_secret, uint32_t index) const
{
  if (index >= _tree.size()) {
    throw InvalidParameterError("Index too large for tree");
  }

  auto path = _tree.encrypt(LeafIndex{ index }, evict_secret);
  return sign(Remove{ LeafIndex{ index }, path });
}

///
/// Message handlers
///

State
State::handle(const MLSPlaintext& handshake) const
{
  if (handshake.epoch != _epoch) {
    throw InvalidParameterError("Epoch mismatch");
  }

  if (handshake.content_type != ContentType::handshake) {
    throw InvalidParameterError("Incorrect content type");
  }

  if (!verify(handshake)) {
    throw ProtocolError("Invalid handshake message signature");
  }

  auto& operation = handshake.operation.value();
  auto next = handle(handshake.sender, operation);

  if (!next.verify_confirmation(operation)) {
    throw InvalidParameterError("Invalid confirmation MAC");
  }

  return next;
}

State
State::handle(LeafIndex signer_index, const GroupOperation& operation) const
{
  auto next = *this;
  bytes update_secret;
  switch (operation.type) {
    case GroupOperationType::add:
      update_secret = next.handle(operation.add.value());
      break;
    case GroupOperationType::update:
      update_secret = next.handle(signer_index, operation.update.value());
      break;
    case GroupOperationType::remove:
      update_secret = next.handle(operation.remove.value());
      break;
  }

  next.update_transcript_hash(operation);
  next._epoch += 1;
  next.update_epoch_secrets(update_secret);

  return next;
}

bytes
State::handle(const Add& add)
{
  // Verify the UserInitKey in the Add message
  if (!add.init_key.verify()) {
    throw InvalidParameterError("Invalid signature on init key in group add");
  }

  // Verify the index in the Add message
  if (add.index.val > _tree.size()) {
    throw InvalidParameterError("Invalid leaf index");
  }
  if (add.index.val < _tree.size() && _tree.occupied(add.index)) {
    throw InvalidParameterError("Leaf is not available for add");
  }

  // Verify the WelcomeInfo hash
  if (add.welcome_info_hash != welcome_info().hash(_suite)) {
    throw ProtocolError("Mismatch in welcome info hash");
  }

  // Add to the tree
  auto init_key = add.init_key.find_init_key(_suite);
  if (!init_key) {
    throw ProtocolError("New node does not support group's cipher suite");
  }
  _tree.add_leaf(add.index, *init_key, add.init_key.credential);

  return _zero;
}

bytes
State::handle(LeafIndex index, const Update& update)
{
  std::optional<bytes> leaf_secret = std::nullopt;
  if (index == _index) {
    if (_cached_leaf_secret.empty()) {
      throw InvalidParameterError("Got self-update without generating one");
    }

    leaf_secret = _cached_leaf_secret;
    _cached_leaf_secret.clear();
  }

  return update_leaf(index, update.path, leaf_secret);
}

bytes
State::handle(const Remove& remove)
{
  auto leaf_secret = std::nullopt;
  auto update_secret = update_leaf(remove.removed, remove.path, leaf_secret);
  _tree.blank_path(remove.removed);

  auto cut = _tree.leaf_span();
  _tree.truncate(cut);

  return update_secret;
}

State::EpochSecrets
State::derive_epoch_secrets(CipherSuite suite,
                            const bytes& init_secret,
                            const bytes& update_secret,
                            const bytes& group_state)
{
  auto epoch_secret = hkdf_extract(suite, init_secret, update_secret);
  return {
    epoch_secret,
    derive_secret(suite, epoch_secret, "app", group_state),
    derive_secret(suite, epoch_secret, "hs", group_state),
    derive_secret(suite, epoch_secret, "sender", group_state),
    derive_secret(suite, epoch_secret, "confirm", group_state),
    derive_secret(suite, epoch_secret, "init", group_state),
  };
}

///
/// Message protection
///

MLSCiphertext
State::protect(const bytes& data)
{
  MLSPlaintext pt{ _epoch, _index, data };
  pt.sign(_identity_priv);

  return encrypt(pt);
}

bytes
State::unprotect(const MLSCiphertext& ct)
{
  MLSPlaintext pt = decrypt(ct);

  if (!verify(pt)) {
    throw ProtocolError("Invalid message signature");
  }

  if (pt.content_type != ContentType::application) {
    throw ProtocolError("Unprotect of non-application message");
  }

  return pt.application_data.value();
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
  auto transcript_hash = (lhs._transcript_hash == rhs._transcript_hash);
  auto group_state = (lhs._group_state == rhs._group_state);

  auto epoch_secret = (lhs._epoch_secret == rhs._epoch_secret);
  auto application_secret =
    (lhs._application_secret == rhs._application_secret);
  auto confirmation_key = (lhs._confirmation_key == rhs._confirmation_key);
  auto init_secret = (lhs._init_secret == rhs._init_secret);

  return suite && group_id && epoch && tree && transcript_hash && group_state &&
         epoch_secret && application_secret && confirmation_key && init_secret;
}

bool
operator!=(const State& lhs, const State& rhs)
{
  return !(lhs == rhs);
}

WelcomeInfo
State::welcome_info() const
{
  return { _group_id, _epoch, _tree, _transcript_hash, _init_secret };
}

void
State::update_transcript_hash(const GroupOperation& operation)
{
  auto operation_bytes = operation.for_transcript();
  _transcript_hash =
    Digest(_suite).write(_transcript_hash).write(operation_bytes).digest();
}

bytes
State::update_leaf(LeafIndex index,
                   const DirectPath& path,
                   const std::optional<bytes>& leaf_secret)
{
  bytes update_secret;
  if (leaf_secret.has_value()) {
    update_secret = _tree.set_path(index, *leaf_secret);
  } else {
    auto merge_path = _tree.decrypt(index, path);
    update_secret = merge_path.root_path_secret;
    _tree.merge_path(index, merge_path);
  }

  return update_secret;
}

void
State::update_epoch_secrets(const bytes& update_secret)
{
  auto group_state_str = GroupState{
    _group_id,
    _epoch,
    _tree.root_hash(),
    _transcript_hash,
  };
  _group_state = tls::marshal(group_state_str);

  auto secrets =
    derive_epoch_secrets(_suite, _init_secret, update_secret, _group_state);
  _epoch_secret = secrets.epoch_secret;
  _application_secret = secrets.application_secret;
  _handshake_secret = secrets.handshake_secret;
  _sender_data_secret = secrets.sender_data_secret;
  _confirmation_key = secrets.confirmation_key;
  _init_secret = secrets.init_secret;

  _application_keys.start(_index, _application_secret);
  _handshake_keys.start(_index, _handshake_secret);
}

///
/// Message encryption and decryption
///

// struct {
//     opaque group_id<0..255>;
//     uint32 epoch;
//     uint32 sender;
//     ContentType content_type;
//     uint32 generation;
// } MLSContentAAD;
static bytes
content_aad(const bytes& group_id,
            uint32_t epoch,
            LeafIndex sender,
            ContentType content_type,
            uint32_t generation)
{
  tls::ostream wc;
  tls::opaque<1> tls_group_id = group_id;
  wc << tls_group_id << epoch << sender << content_type << generation;
  return wc.bytes();
}

struct SenderData
{
  LeafIndex sender;
  ContentType content_type;
  uint32_t generation;
};

static tls::ostream&
operator<<(tls::ostream& out, const SenderData& obj)
{
  return out << obj.sender << obj.content_type << obj.generation;
}

static tls::istream&
operator>>(tls::istream& in, SenderData& obj)
{
  return in >> obj.sender >> obj.content_type >> obj.generation;
}

// sample = ciphertext[:Hash.length]
// mask = HMAC(sender_data_secret, sample)[:9]
static bytes
sender_data_mask(CipherSuite suite,
                 const bytes& sender_data_secret,
                 const bytes& ciphertext)
{
  const size_t sender_data_size = 9;
  size_t sample_size = Digest(suite).output_size();
  if (sample_size > ciphertext.size()) {
    sample_size = ciphertext.size();
  }
  auto sample = bytes(ciphertext.begin(), ciphertext.begin() + sample_size);
  auto mask = hmac(suite, sender_data_secret, sample);
  mask.resize(sender_data_size);
  return mask;
}

std::tuple<MLSPlaintext, State>
State::sign(const GroupOperation& operation) const
{
  auto next = handle(_index, operation);
  auto confirm = hmac(_suite, next._confirmation_key, next._transcript_hash);

  auto pt = MLSPlaintext{ _suite };
  pt.epoch = _epoch;
  pt.sender = _index;
  pt.content_type = ContentType::handshake;
  pt.operation = operation;
  pt.operation.value().confirmation = confirm;
  pt.sign(_identity_priv);
  return std::make_tuple(pt, next);
}

bool
State::verify(const MLSPlaintext& pt) const
{
  auto pub = _tree.get_credential(pt.sender).public_key();
  return pt.verify(pub);
}

bool
State::verify_confirmation(const GroupOperation& operation) const
{
  auto confirm = hmac(_suite, _confirmation_key, _transcript_hash);
  return constant_time_eq(confirm, operation.confirmation);
}

MLSCiphertext
State::encrypt(const MLSPlaintext& pt)
{
  // Pull from the key schedule
  KeyChain::Generation keys;
  switch (pt.content_type) {
    case ContentType::handshake:
      keys = _handshake_keys.next();
      break;

    case ContentType::application:
      keys = _application_keys.next();
      break;

    default:
      throw InvalidParameterError("Unknown content type");
  }

  // Compute the plaintext input and AAD
  // XXX(rlb@ipv.sx): Apply padding?
  auto content = pt.marshal_content(0);
  auto aad =
    content_aad(_group_id, _epoch, _index, pt.content_type, keys.generation);

  // Encrypt the plaintext
  auto gcm = AESGCM(keys.key, keys.nonce);
  gcm.set_aad(aad);
  auto ciphertext = gcm.encrypt(content);

  // Compute and mask the sender data
  // TODO generate sender_data_secret
  auto sender_data =
    tls::marshal(SenderData{ _index, pt.content_type, keys.generation });
  auto mask = sender_data_mask(_suite, _sender_data_secret, ciphertext);
  auto masked_sender_data = sender_data ^ mask;

  // Assemble the MLSCiphertext
  MLSCiphertext ct;
  ct.epoch = _epoch;
  std::copy(masked_sender_data.begin(),
            masked_sender_data.end(),
            ct.masked_sender_data.begin());
  ct.ciphertext = ciphertext;
  return ct;
}

MLSPlaintext
State::decrypt(const MLSCiphertext& ct) const
{
  // Verify the epoch
  if (ct.epoch != _epoch) {
    throw InvalidParameterError("Ciphertext not from this epoch");
  }

  // Unmask, parse, and validate the sender data
  auto mask = sender_data_mask(_suite, _sender_data_secret, ct.ciphertext);
  auto masked_sender_data =
    bytes(ct.masked_sender_data.begin(), ct.masked_sender_data.end());
  auto sender_data_data = masked_sender_data ^ mask;
  SenderData sender_data;
  tls::unmarshal(sender_data_data, sender_data);
  if (!_tree.occupied(sender_data.sender)) {
    throw ProtocolError("Encryption from unoccupied leaf");
  }

  // Pull from the key schedule
  KeyChain::Generation keys;
  switch (sender_data.content_type) {
    case ContentType::handshake:
      keys = _handshake_keys.get(sender_data.sender, sender_data.generation);
      break;

    case ContentType::application:
      keys = _application_keys.get(sender_data.sender, sender_data.generation);
      break;

    default:
      throw InvalidParameterError("Unknown content type");
  }

  // Compute the plaintext AAD and decrypt
  auto aad = content_aad(_group_id,
                         _epoch,
                         sender_data.sender,
                         sender_data.content_type,
                         sender_data.generation);
  auto gcm = AESGCM(keys.key, keys.nonce);
  gcm.set_aad(aad);
  auto content = gcm.decrypt(ct.ciphertext);

  // Set up a template plaintext and parse into it
  auto pt = MLSPlaintext{ _suite };
  pt.epoch = _epoch;
  pt.sender = sender_data.sender;
  pt.content_type = sender_data.content_type;
  pt.unmarshal_content(_suite, content);
  return pt;
}

} // namespace mls
