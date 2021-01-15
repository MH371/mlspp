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
    mls::MLSPlaintext commit;
    CryptoValue tree_hash;

    CryptoValue commit_secret;
    CryptoValue psk_secret;

    CryptoValue confirmed_transcript_hash;
    CryptoValue interim_transcript_hash;
    CryptoValue group_context;

    CryptoValue joiner_secret;
    CryptoValue welcome_secret;
    CryptoValue epoch_secret;
    CryptoValue init_secret;

    CryptoValue sender_data_secret;
    CryptoValue encryption_secret;
    CryptoValue exporter_secret;
    CryptoValue authentication_secret;
    CryptoValue external_secret;
    CryptoValue confirmation_key;
    CryptoValue membership_key;
    CryptoValue resumption_secret;

    mls::HPKEPublicKey external_pub;

    TLS_SERIALIZABLE(commit,
                     tree_hash,
                     commit_secret,
                     psk_secret,
                     confirmed_transcript_hash,
                     interim_transcript_hash,
                     group_context,
                     joiner_secret,
                     welcome_secret,
                     epoch_secret,
                     init_secret,
                     sender_data_secret,
                     encryption_secret,
                     exporter_secret,
                     authentication_secret,
                     external_secret,
                     confirmation_key,
                     membership_key,
                     resumption_secret,
                     external_pub)
  };

  mls::CipherSuite suite;
  CryptoValue group_id;
  CryptoValue initial_tree_hash;
  CryptoValue initial_init_secret;
  std::vector<Epoch> epochs;

  TLS_SERIALIZABLE(suite,
                   group_id,
                   initial_tree_hash,
                   initial_init_secret,
                   epochs)
  TLS_TRAITS(tls::pass, tls::pass, tls::pass, tls::pass, tls::vector<4>)

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
