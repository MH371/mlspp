#include "messages.h"

#include <iostream>

#define DUMMY_CIPHERSUITE CipherSuite::P256_SHA256_AES128GCM

namespace mls {

// RatchetNode

RatchetNode::RatchetNode(CipherSuite suite)
  : CipherAware(suite)
  , public_key(suite)
  , node_secrets(suite)
{}

RatchetNode::RatchetNode(DHPublicKey public_key_in,
                         const std::vector<HPKECiphertext>& node_secrets_in)
  : CipherAware(public_key_in.cipher_suite())
  , public_key(std::move(public_key_in))
  , node_secrets(node_secrets_in)
{}

// DirectPath

DirectPath::DirectPath(CipherSuite suite)
  : CipherAware(suite)
  , nodes(suite)
{}

// ClientInitKey

ClientInitKey::ClientInitKey()
  : supported_versions(1, ProtocolVersion::mls10)
{}

ClientInitKey::ClientInitKey(
  bytes client_init_key_id_in,
  const std::vector<CipherSuite>& supported_ciphersuites,
  const bytes& init_secret,
  const Credential& credential_in)
  : client_init_key_id(std::move(client_init_key_id_in))
  , supported_versions(1, ProtocolVersion::mls10)
{
  // XXX(rlb@ipv.sx) - It's probably not OK to derive all the keys
  // from the same secret.  Maybe we should include the ciphersuite
  // in the key derivation...
  //
  // Note, though, that since ClientInitKey objects track private
  // keys, it would be safe to just generate keys here, if we were
  // OK having internal keygen.
  for (const auto suite : supported_ciphersuites) {
    auto init_priv = DHPrivateKey::derive(suite, init_secret);
    add_init_key(init_priv);
  }

  sign(credential_in);
}

void
ClientInitKey::add_init_key(const DHPrivateKey& priv)
{
  auto suite = priv.cipher_suite();
  cipher_suites.push_back(suite);
  init_keys.push_back(priv.public_key().to_bytes());
  _private_keys.emplace(suite, priv);
}

std::optional<DHPublicKey>
ClientInitKey::find_init_key(CipherSuite suite) const
{
  for (size_t i = 0; i < cipher_suites.size(); ++i) {
    if (cipher_suites[i] == suite) {
      return DHPublicKey{ suite, init_keys[i] };
    }
  }

  return std::nullopt;
}

std::optional<DHPrivateKey>
ClientInitKey::find_private_key(CipherSuite suite) const
{
  if (_private_keys.count(suite) == 0) {
    return std::nullopt;
  }

  return _private_keys.at(suite);
}

void
ClientInitKey::sign(const Credential& credential_in)
{
  if (!credential_in.private_key().has_value()) {
    throw InvalidParameterError("Credential must have a private key");
  }
  auto identity_priv = credential_in.private_key().value();

  if (cipher_suites.size() != init_keys.size()) {
    throw InvalidParameterError("Mal-formed ClientInitKey");
  }

  credential = credential_in;

  auto tbs = to_be_signed();
  signature = identity_priv.sign(tbs);
}

bool
ClientInitKey::verify() const
{
  auto tbs = to_be_signed();
  auto identity_key = credential.public_key();
  return identity_key.verify(tbs, signature);
}

bytes
ClientInitKey::to_be_signed() const
{
  tls::ostream out;
  out << cipher_suites << init_keys << credential;
  return out.bytes();
}

// WelcomeInfo

WelcomeInfo::WelcomeInfo(CipherSuite suite)
  : CipherAware(suite)
  , version(ProtocolVersion::mls10)
  , epoch(0)
  , tree(suite)
{}

WelcomeInfo::WelcomeInfo(tls::opaque<2> group_id_in,
                         epoch_t epoch_in,
                         RatchetTree tree_in,
                         const tls::opaque<1>& interim_transcript_hash_in,
                         const tls::opaque<1>& init_secret_in)
  : CipherAware(tree_in.cipher_suite())
  , version(ProtocolVersion::mls10)
  , group_id(std::move(group_id_in))
  , epoch(epoch_in)
  , tree(std::move(tree_in))
  , interim_transcript_hash(interim_transcript_hash_in)
  , init_secret(init_secret_in)
{}

bytes
WelcomeInfo::hash(CipherSuite suite) const
{
  auto marshaled = tls::marshal(*this);
  return Digest(suite).write(marshaled).digest();
}

// Welcome

Welcome::Welcome()
  : cipher_suite(DUMMY_CIPHERSUITE)
  , encrypted_welcome_info(DUMMY_CIPHERSUITE)
{}

Welcome::Welcome(const bytes& id,
                 const DHPublicKey& pub,
                 const WelcomeInfo& info)
  : client_init_key_id(id)
  , cipher_suite(pub.cipher_suite())
  , encrypted_welcome_info(pub.encrypt(tls::marshal(info)))
{}

WelcomeInfo
Welcome::decrypt(const DHPrivateKey& priv) const
{
  auto welcome_info_bytes = priv.decrypt(encrypted_welcome_info);
  auto welcome_info = WelcomeInfo{ priv.cipher_suite() };
  tls::unmarshal(welcome_info_bytes, welcome_info);
  return welcome_info;
}

bool
operator==(const Welcome& lhs, const Welcome& rhs)
{
  return (lhs.client_init_key_id == rhs.client_init_key_id) &&
         (lhs.cipher_suite == rhs.cipher_suite) &&
         (lhs.encrypted_welcome_info == rhs.encrypted_welcome_info);
}

tls::ostream&
operator<<(tls::ostream& out, const Welcome& obj)
{
  return out << obj.client_init_key_id << obj.cipher_suite
             << obj.encrypted_welcome_info;
}

tls::istream&
operator>>(tls::istream& in, Welcome& obj)
{
  in >> obj.client_init_key_id >> obj.cipher_suite;

  obj.encrypted_welcome_info = HPKECiphertext{ obj.cipher_suite };
  in >> obj.encrypted_welcome_info;
  return in;
}

// Add

Add::Add(LeafIndex index_in,
         ClientInitKey init_key_in,
         bytes welcome_info_hash_in)
  : index(index_in)
  , init_key(std::move(init_key_in))
  , welcome_info_hash(std::move(welcome_info_hash_in))
{}

const GroupOperationType Add::type = GroupOperationType::add;

// Update

Update::Update(CipherSuite suite)
  : CipherAware(suite)
  , path(suite)
{}

Update::Update(const DirectPath& path_in)
  : CipherAware(path_in.cipher_suite())
  , path(path_in)
{}

const GroupOperationType Update::type = GroupOperationType::update;

// Remove

Remove::Remove(CipherSuite suite)
  : CipherAware(suite)
  , path(suite)
{}

Remove::Remove(LeafIndex removed_in, const DirectPath& path_in)
  : CipherAware(path_in.cipher_suite())
  , removed(removed_in)
  , path(path_in)
{}

const GroupOperationType Remove::type = GroupOperationType::remove;

// GroupOperation

GroupOperation::GroupOperation(CipherSuite suite)
  : CipherAware(suite)
  , InnerOp(suite, Add{})
{}

GroupOperation::GroupOperation(const Add& add)
  : CipherAware(DUMMY_CIPHERSUITE)
  , InnerOp(DUMMY_CIPHERSUITE, add)
{}

GroupOperation::GroupOperation(const Update& update)
  : CipherAware(update.cipher_suite())
  , InnerOp(update.cipher_suite(), update)

{}

GroupOperation::GroupOperation(const Remove& remove)
  : CipherAware(remove.cipher_suite())
  , InnerOp(remove.cipher_suite(), remove)
{}

// MLSPlaintext

const ContentType HandshakeData::type = ContentType::handshake;
const ContentType ApplicationData::type = ContentType::application;

MLSPlaintext::MLSPlaintext(CipherSuite suite)
  : CipherAware(suite)
  , content(suite, ApplicationData{ suite })
{}

MLSPlaintext::MLSPlaintext(CipherSuite suite,
                           bytes group_id_in,
                           epoch_t epoch_in,
                           LeafIndex sender_in,
                           ContentType content_type_in,
                           bytes content_in)
  : CipherAware(suite)
  , group_id(group_id_in)
  , epoch(epoch_in)
  , sender(sender_in)
  , content(suite, ApplicationData{ suite })
{
  int cut = content_in.size() - 1;
  for (; content_in[cut] == 0 && cut >= 0; cut -= 1) {
  }
  if (content_in[cut] != 0x01) {
    throw ProtocolError("Invalid marker byte");
  }

  uint16_t sig_len;
  auto start = content_in.begin();
  auto sig_len_bytes = bytes(start + cut - 2, start + cut);
  tls::unmarshal(sig_len_bytes, sig_len);
  cut -= 2;
  if (sig_len > cut) {
    throw ProtocolError("Invalid signature size");
  }

  signature = bytes(start + cut - sig_len, start + cut);
  auto content_data = bytes(start, start + cut - sig_len);

  switch (content_type_in) {
    case ContentType::handshake: {
      auto& operation = content.emplace<HandshakeData>(suite);
      tls::unmarshal(content_data, operation);
      break;
    }

    case ContentType::application: {
      auto& application_data = content.emplace<ApplicationData>();
      tls::unmarshal(content_data, application_data);
      break;
    }

    default:
      throw InvalidParameterError("Unknown content type");
  }
}

MLSPlaintext::MLSPlaintext(bytes group_id_in,
                           epoch_t epoch_in,
                           LeafIndex sender_in,
                           const GroupOperation& operation_in)
  : CipherAware(operation_in.cipher_suite())
  , group_id(std::move(group_id_in))
  , epoch(epoch_in)
  , sender(sender_in)
  , content(operation_in.cipher_suite(), HandshakeData{ operation_in, {} })
{}

MLSPlaintext::MLSPlaintext(bytes group_id_in,
                           epoch_t epoch_in,
                           LeafIndex sender_in,
                           const ApplicationData& application_data_in)
  : CipherAware(DUMMY_CIPHERSUITE)
  , group_id(std::move(group_id_in))
  , epoch(epoch_in)
  , sender(sender_in)
  , content(DUMMY_CIPHERSUITE, application_data_in)
{}

// struct {
//     opaque content[MLSPlaintext.length];
//     uint8 signature[MLSInnerPlaintext.sig_len];
//     uint16 sig_len;
//     uint8  marker = 1;
//     uint8  zero\_padding[length\_of\_padding];
// } MLSContentPlaintext;
bytes
MLSPlaintext::marshal_content(size_t padding_size) const
{
  bytes marshaled;
  if (content.type() == ContentType::handshake) {
    marshaled = tls::marshal(std::get<HandshakeData>(content));
  } else if (content.type() == ContentType::application) {
    marshaled = tls::marshal(std::get<ApplicationData>(content));
  } else {
    throw InvalidParameterError("Unknown content type");
  }

  uint16_t sig_len = signature.size();
  auto marker = bytes{ 0x01 };
  auto pad = zero_bytes(padding_size);
  marshaled = marshaled + signature + tls::marshal(sig_len) + marker + pad;
  return marshaled;
}

// struct {
//   opaque group_id<0..255>;
//   uint32 epoch;
//   uint32 sender;
//   ContentType content_type = handshake;
//   GroupOperation operation;
// } MLSPlaintextOpContent;
bytes
MLSPlaintext::op_content() const
{
  auto& handshake_data = std::get<HandshakeData>(content);
  tls::ostream w;
  w << group_id << epoch << sender << content.type()
    << handshake_data.operation;
  return w.bytes();
}

// struct {
//   opaque confirmation<0..255>;
//   opaque signature<0..2^16-1>;
// } MLSPlaintextOpAuthData;
bytes
MLSPlaintext::auth_data() const
{
  auto& handshake_data = std::get<HandshakeData>(content);
  tls::ostream w;
  w << handshake_data.confirmation << signature;
  return w.bytes();
}

bytes
MLSPlaintext::to_be_signed() const
{
  tls::ostream w;
  w << group_id << epoch << sender << content;
  return w.bytes();
}

void
MLSPlaintext::sign(const SignaturePrivateKey& priv)
{
  auto tbs = to_be_signed();
  std::cout << "sig: " << tbs << std::endl;
  signature = priv.sign(tbs);
}

bool
MLSPlaintext::verify(const SignaturePublicKey& pub) const
{
  auto tbs = to_be_signed();
  std::cout << "ver: " << tbs << std::endl;
  return pub.verify(tbs, signature);
}

} // namespace mls
