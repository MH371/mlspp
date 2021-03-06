include(BuildUtility)
configure_file(lib/hpke/src/common.cpp ${CMAKE_CURRENT_SOURCE_DIR}/lib/hpke/src/hpke_common.cpp COPYONLY)
BuildUtility(
    NAME mlspp
    IGNORE_WARNINGS_MSVC
       4244 # 'conversion' conversion from 'type1' to 'type2', possible loss of data
    FILES
       src/common.cpp
       src/core_types.cpp
       src/credential.cpp
       src/crypto.cpp
       src/key_schedule.cpp
       src/log.cpp
       src/messages.cpp
       src/session.cpp
       src/state.cpp
       src/tree_math.cpp
       src/treekem.cpp
       lib/bytes/src/bytes.cpp
       lib/hpke/src/aead_cipher.cpp
       lib/hpke/src/certificate.cpp
       lib/hpke/src/hpke_common.cpp
       lib/hpke/src/dhkem.cpp
       lib/hpke/src/digest.cpp
       lib/hpke/src/group.cpp
       lib/hpke/src/hkdf.cpp
       lib/hpke/src/hpke.cpp
       lib/hpke/src/openssl_common.cpp
       lib/hpke/src/random.cpp
       lib/hpke/src/rsa.cpp
       lib/hpke/src/signature.cpp
       lib/mls_vectors/src/mls_vectors.cpp
       lib/tls_syntax/src/tls_syntax.cpp
    PUBLIC_HEADERS_DIR
       include
       lib/bytes/include
       lib/hpke/include
       lib/mls_vectors/include
       lib/mpark-variant/include
       lib/tls_syntax/include
    DEPENDS_ON
       openssl
)
