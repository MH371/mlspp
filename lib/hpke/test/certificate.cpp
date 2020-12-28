#include <doctest/doctest.h>
#include <hpke/certificate.h>

#include "common.h"

#include <fstream>
#include <iostream>
#include <vector>

TEST_CASE("Certificate Known-Answer depth 2")
{
  // TODO(suhas) Do this for each supported signature algorithm
  //      ... maybe including a case where parent and child have different
  //      algorithms
  // TODO(suhas) create different cert chains based on depth and algo

  // Chain is of depth 2
  const auto root_der = from_hex(
    "308201083081bba003020102021066144f6b1f7f06eaa3c5c4a24cdfb86f300506032b65"
    "7030143112301006035504031309746573742e636d6f6d301e170d323031303036303231"
    "3234395a170d3230313030373032313234395a3014311230100603550403130974657374"
    "2e636d6f6d302a300506032b65700321001afc1fc100f32f8abb6e7e1635eb873aba8583"
    "b8af948fb07e4b20376a8a89bba3233021300e0603551d0f0101ff0404030202a4300f06"
    "03551d130101ff040530030101ff300506032b6570034100a45de2d187cb28b4a74a4e82"
    "e4a000d68176ae68250803666d3a92b6595b0b0fbdcf231f83542fe29b74a95912a6b71b"
    "8e967f07df14b01b2b4779b233669e02");
  const auto issuing_der = from_hex(
    "3082010d3081c0a00302010202100cf69c4bee7caddc4bdcbd83d7f4e3e5300506032b65"
    "7030143112301006035504031309746573742e636d6f6d301e170d323031303036303231"
    "3234395a170d3230313030373032313234395a30193117301506035504030c0e022e696e"
    "742e746573742e636f6d302a300506032b6570032100ae385adcf6abd78d72bbcc1fdc94"
    "7903875e106c321a63d1367218d654356fd3a3233021300e0603551d0f0101ff04040302"
    "02a4300f0603551d130101ff040530030101ff300506032b65700341007520273c7a32fd"
    "b790c75e7925e53fb32c07add36f1d5c947209865d9ba88a7b2639466d3cf37e69df5667"
    "9d74700077906570c3690f74d944eaa2779ae0cb09");
  const auto leaf_der = from_hex(
    "3081f73081aaa003020102021100fad304f1a5a78be09d01347ed82a04e2300506032b65"
    "7030193117301506035504030c0e022e696e742e746573742e636f6d301e170d32303130"
    "30363032313234395a170d3230313030373032313234395a3000302a300506032b657003"
    "2100f8659f1bbfd057370f86c13c4dbe6850d2184a1b1a899d2d277a54d3666d7625a320"
    "301e300e0603551d0f0101ff0404030202a4300c0603551d130101ff0402300030050603"
    "2b6570034100ec538e976a425f1606e0e3d1f92599ab4a37fdd4deb07d3cf61a1f0f1867"
    "a0518253806c85a793ef5619b5803d4bc72a253a46f770acd65ae6907627e6852002");

  auto root = Certificate{ root_der };
  auto issuing = Certificate{ issuing_der };
  auto leaf = Certificate{ leaf_der };

  REQUIRE(!root.hash().empty());
  REQUIRE(!leaf.hash().empty());
  REQUIRE(!issuing.hash().empty());

  CHECK(root.raw == root_der);
  CHECK(issuing.raw == issuing_der);
  CHECK(leaf.raw == leaf_der);

  CHECK(leaf.valid_from(issuing));
  CHECK(issuing.valid_from(root));
  CHECK(root.valid_from(root));

  CHECK(!leaf.is_ca());
  CHECK(issuing.is_ca());
  CHECK(root.is_ca());

  REQUIRE(root.subject() == 1072860458);
  REQUIRE(issuing.issuer() == root.subject());
  REQUIRE(leaf.issuer() == issuing.subject());

  // negative tests
  CHECK_FALSE(issuing.valid_from(leaf));
  CHECK_FALSE(root.valid_from(issuing));
  CHECK_FALSE(root.valid_from(leaf));
}

TEST_CASE("Certificate Known-Answer depth 2 with SKID/ADID")
{
  const auto root_der = from_hex(
    "308201183081cba0030201020211009561abf361bd738664041a79d918f602300506032b"
    "657030143112301006035504031309746573742e636d6f6d301e170d3230313030363035"
    "303433365a170d3230313030373035303433365a30143112301006035504031309746573"
    "742e636d6f6d302a300506032b657003210047f0149110ed81e2beaabbc3699527bdb8b7"
    "45da010da7fb8301d06fff8239e4a3323030300e0603551d0f0101ff0404030202a4300f"
    "0603551d130101ff040530030101ff300d0603551d0e04060404b9e672b8300506032b65"
    "70034100e15b54d50d1354f44017c5f8a037228546256c5fa1d750758fdf76f7e1dc246e"
    "7c67c18226ffd6704327bbae9a0cf5bd209facdcb524dc7efa517d1155487a0e");
  const auto issuing_der = from_hex(
    "3082012e3081e1a003020102021100919fc6cb4ea1a73766c95ada9a476aee300506032b"
    "657030143112301006035504031309746573742e636d6f6d301e170d3230313030363035"
    "303433365a170d3230313030373035303433365a30193117301506035504030c0e022e69"
    "6e742e746573742e636f6d302a300506032b6570032100c0a9d461880d82db662e984a8c"
    "06f74479817952070ea8cd0010971bc793ab9ca3433041300e0603551d0f0101ff040403"
    "0202a4300f0603551d130101ff040530030101ff300d0603551d0e0406040475ecb84430"
    "0f0603551d23040830068004b9e672b8300506032b6570034100e72bf92a39bab96649ab"
    "bd619c47b054bf7b071ceb24710ad253682d1df7690b0a1ef28ef8a2f76c3b8f2fed9d11"
    "3a61c98768db30d16fe6d9a36595775d110e");
  const auto leaf_der = from_hex(
    "308201163081c9a0030201020210291fb8fb96d2c215cb2d532f252d80e4300506032b65"
    "7030193117301506035504030c0e022e696e742e746573742e636f6d301e170d32303130"
    "30363035303433365a170d3230313030373035303433365a3000302a300506032b657003"
    "210032e4be5553d2141ace4da105fdf632da3467f013581f57dbd4f09706fa99949da340"
    "303e300e0603551d0f0101ff0404030202a4300c0603551d130101ff04023000300d0603"
    "551d0e04060404dd3b0790300f0603551d2304083006800475ecb844300506032b657003"
    "4100314a485d01df4c7852ec5720b2af34f5620b2a32a50c4ee0481d013ebbfd8e243784"
    "123a0cfe4d59b1fb09a1738ee9bc2aab59a2b4af2c3ee60ce19afbe1eb03");

  const auto root_skid = std::string("b9e672b8");
  const auto issuing_skid = std::string("75ecb844");
  const auto leaf_skid = std::string("dd3b0790");

  auto root = Certificate{ root_der };
  auto issuing = Certificate{ issuing_der };
  auto leaf = Certificate{ leaf_der };

  CHECK(root.raw == root_der);
  CHECK(issuing.raw == issuing_der);
  CHECK(leaf.raw == leaf_der);

  CHECK(leaf.valid_from(issuing));
  CHECK(issuing.valid_from(root));
  CHECK(root.valid_from(root));

  REQUIRE(leaf.subject_key_id().has_value());
  REQUIRE(leaf.authority_key_id().has_value());

  REQUIRE(issuing.subject_key_id().has_value());
  REQUIRE(issuing.authority_key_id().has_value());

  REQUIRE(root.subject_key_id().has_value());

  CHECK_EQ(to_hex(leaf.subject_key_id().value()), leaf_skid);
  CHECK_EQ(to_hex(leaf.authority_key_id().value()),
           to_hex(issuing.subject_key_id().value()));
  CHECK_EQ(to_hex(issuing.subject_key_id().value()), issuing_skid);
  CHECK_EQ(to_hex(issuing.authority_key_id().value()),
           to_hex(root.subject_key_id().value()));
  CHECK_EQ(to_hex(root.subject_key_id().value()), root_skid);
}

TEST_CASE("Certificate Known-Answer depth 2 with SAN RFC822Name")
{
  const auto root_der = from_hex(
    "3081ff3081b2a00302010202101025963d9aefe2cdaf9c8017b9836b9b300506032b657030"
    "00301e170d3230313132353232333135365a170d3230313132363232333135365a3000302a"
    "300506032b65700321006fd52c993c4554c550c6f57a8c9b44834a99889c882e597d78e952"
    "afdbde748ea3423040300e0603551d0f0101ff0404030202a4300f0603551d130101ff0405"
    "30030101ff301d0603551d110101ff04133011810f7573657240646f6d61696e2e636f6d30"
    "0506032b6570034100accb5e7e05e607ca0c5a9103e962e360ea0b95ab8c876993af2660ef"
    "7e22ae6714f3d7b6b9594ac3eaaeeef263f764bc4939c84005db311ac4740b665694b004");
  const auto issuing_der = from_hex(
    "3081ff3081b2a0030201020210277bfa0157eaa84f1dc14c07ade455dd300506032b657030"
    "00301e170d3230313132353232333135365a170d3230313132363232333135365a3000302a"
    "300506032b65700321005ddafa25a2313f8dd19be29736825207a67282c2c6e327b8ac5127"
    "102e0d4eeda3423040300e0603551d0f0101ff0404030202a4300f0603551d130101ff0405"
    "30030101ff301d0603551d110101ff04133011810f7573657240646f6d61696e2e636f6d30"
    "0506032b6570034100eea828a18197fd4bd5751959318a7def21ce0c588b4107dc51ab6eb3"
    "e1a0a7c440cc019c186fbdbe227c0f368ab993c8a5af5c9681e11583d0442cafcaf01300");
  const auto leaf_der = from_hex(
    "3081fd3081b0a003020102021100af5442db77d60c749fffe8eebf193afa300506032b6570"
    "3000301e170d3230313132353232333135365a170d3230313132363232333135365a300030"
    "2a300506032b6570032100885cc6836723e204b54275c97928481c55b149e1ed0e22b30d2f"
    "1a89aa24e2d1a33f303d300e0603551d0f0101ff0404030202a4300c0603551d130101ff04"
    "023000301d0603551d110101ff04133011810f7573657240646f6d61696e2e636f6d300506"
    "032b65700341002cc5b3f1a8954ccc872ecddf5779fb007c08ebc869227dec09cfba8fd977"
    "ea49a182a2e51b67d4440d42248f6951f4c765e9e72e301225c953e89b2747129a0c");

  auto root = Certificate{ root_der };
  auto issuing = Certificate{ issuing_der };
  auto leaf = Certificate{ leaf_der };

  CHECK(leaf.valid_from(issuing));
  CHECK(issuing.valid_from(root));
  CHECK(root.valid_from(root));

  CHECK_EQ(leaf.email_addresses().at(0), "user@domain.com");
}