
add_subdirectory(lib)

include(BuildUtility)

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
    PUBLIC_HEADERS_DIR
       include
    DEPENDS_ON_PUBLIC
       bytes
       tls_syntax
       hpke
       mpark-variant
    DEPENDS_ON
       openssl
)
