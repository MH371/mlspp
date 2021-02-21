include(BuildUtility)

BuildUtility(
    NAME hpke
    FILES
       src/aead_cipher.cpp
       src/certificate.cpp
       src/common.cpp
       src/dhkem.cpp
       src/digest.cpp
       src/group.cpp
       src/hkdf.cpp
       src/hpke.cpp
       src/openssl_common.cpp
       src/random.cpp
       src/rsa.cpp
       src/signature.cpp
    PUBLIC_HEADERS_DIR
       include
    PRIVATE_HEADERS_DIR
       src
    DEPENDS_ON
       bytes
       tls_syntax
       openssl
       mpark-variant
)
  
