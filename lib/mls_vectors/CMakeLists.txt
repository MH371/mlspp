include(BuildUtility)

BuildUtility(
    NAME mls_vectors
    IGNORE_WARNINGS_MSVC
       4244 # 'conversion' conversion from 'type1' to 'type2', possible loss of data
    FILES
       src/mls_vectors.cpp
    PUBLIC_HEADERS_DIR
       include
    DEPENDS_ON
       bytes
       tls_syntax
       mlspp
)
