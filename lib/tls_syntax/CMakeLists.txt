include(BuildUtility)

BuildUtility(
    NAME tls_syntax
    IGNORE_WARNINGS_MSVC
       4244 # 'conversion' conversion from 'type1' to 'type2', possible loss of data
    FILES
       src/tls_syntax.cpp
       include/tls/tls_syntax.h
    PUBLIC_HEADERS_DIR
       include
    DEPENDS_ON
       mpark-variant
)
