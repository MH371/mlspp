#pragma once

#include <bytes/bytes.h>
#include <mls/crypto.h>
#include <mls/messages.h>
#include <mls/tree_math.h>
#include <mls/treekem.h>
#include <tls/tls_syntax.h>
#include <vector>

namespace mls_vectors {

struct TreeMathTestVector
{
  using OptionalNode = std::optional<mls::NodeIndex>;

  mls::LeafCount n_leaves;
  mls::NodeCount n_nodes;
  std::vector<mls::NodeIndex> root;
  std::vector<OptionalNode> left;
  std::vector<OptionalNode> right;
  std::vector<OptionalNode> parent;
  std::vector<OptionalNode> sibling;

  static TreeMathTestVector create(uint32_t n_leaves);
  std::optional<std::string> verify() const;
};

struct EncryptionTestVector
{
  struct SenderDataInfo
  {
    bytes ciphertext;
    bytes key;
    bytes nonce;
  };

  struct RatchetStep
  {
    bytes key;
    bytes nonce;
    bytes plaintext;
    bytes ciphertext;
  };

  struct LeafInfo
  {
    uint32_t generations;
    std::vector<RatchetStep> handshake;
    std::vector<RatchetStep> application;
  };

  mls::CipherSuite cipher_suite;
  mls::LeafCount n_leaves;

  bytes encryption_secret;
  bytes sender_data_secret;
  SenderDataInfo sender_data_info;

  std::vector<LeafInfo> leaves;

  static EncryptionTestVector create(mls::CipherSuite suite,
                                     uint32_t n_leaves,
                                     uint32_t n_generations);
  std::optional<std::string> verify() const;
};

struct CryptoValue
{
  bytes data;
  TLS_SERIALIZABLE(data)
  TLS_TRAITS(tls::vector<1>)
};

struct KeyScheduleTestVector
{
  struct Epoch
  {
    // Chosen by the generator
    bytes tree_hash;
    bytes commit_secret;
    bytes psk_secret;
    bytes confirmed_transcript_hash;

    // Computed values
    bytes group_context;

    bytes joiner_secret;
    bytes welcome_secret;
    bytes epoch_secret;
    bytes init_secret;

    bytes sender_data_secret;
    bytes encryption_secret;
    bytes exporter_secret;
    bytes authentication_secret;
    bytes external_secret;
    bytes confirmation_key;
    bytes membership_key;
    bytes resumption_secret;

    mls::HPKEPublicKey external_pub;
  };

  mls::CipherSuite cipher_suite;

  bytes group_id;
  bytes initial_init_secret;

  std::vector<Epoch> epochs;

  static KeyScheduleTestVector create(mls::CipherSuite suite,
                                      uint32_t n_epochs);
  std::optional<std::string> verify() const;
};

struct TreeKEMTestVector
{
  mls::CipherSuite suite;

  mls::TreeKEMPublicKey tree_before;
  CryptoValue tree_hash_before;

  mls::LeafIndex add_sender;
  mls::KeyPackage my_key_package;
  CryptoValue my_path_secret;

  mls::LeafIndex update_sender;
  mls::UpdatePath update_path;

  CryptoValue root_secret;
  mls::TreeKEMPublicKey tree_after;
  CryptoValue tree_hash_after;

  TLS_SERIALIZABLE(suite,
                   tree_before,
                   tree_hash_before,
                   my_key_package,
                   my_path_secret,
                   update_sender,
                   update_path,
                   root_secret,
                   tree_after,
                   tree_hash_after);

  static TreeKEMTestVector create(mls::CipherSuite suite, size_t n_leaves);
  void initialize_trees();
  std::optional<std::string> verify() const;
};

struct MessagesTestVector
{
  struct Message
  {
    bytes data;
    TLS_SERIALIZABLE(data)
    TLS_TRAITS(tls::vector<4>)
  };

  Message key_package;
  Message capabilities;
  Message lifetime;
  Message ratchet_tree;

  Message group_info;
  Message group_secrets;
  Message welcome;

  Message public_group_state;

  Message add_proposal;
  Message update_proposal;
  Message remove_proposal;
  Message pre_shared_key_proposal;
  Message re_init_proposal;
  Message external_init_proposal;
  Message app_ack_proposal;

  Message commit;

  Message mls_plaintext_application;
  Message mls_plaintext_proposal;
  Message mls_plaintext_commit;
  Message mls_ciphertext;

  TLS_SERIALIZABLE(key_package,
                   capabilities,
                   lifetime,
                   ratchet_tree,

                   group_info,
                   group_secrets,
                   welcome,

                   public_group_state,

                   add_proposal,
                   update_proposal,
                   remove_proposal,
                   pre_shared_key_proposal,
                   re_init_proposal,
                   external_init_proposal,
                   app_ack_proposal,

                   commit,

                   mls_plaintext_application,
                   mls_plaintext_proposal,
                   mls_plaintext_commit,
                   mls_ciphertext)

  static MessagesTestVector create();
  std::optional<std::string> verify() const;
};

} // namespace mls_vectors
