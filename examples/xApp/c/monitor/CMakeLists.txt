if(XAPP_DB STREQUAL "MYSQL_XAPP")
	include_directories(${LIBMYSQLCLIENT_INCLUDE_DIRS})
endif()

# xapp_cust_moni
add_executable(xapp_cust_moni
		xapp_cust_moni.c
								${CMAKE_SOURCE_DIR}/src/util/alg_ds/alg/defer.c
								)

target_link_libraries(xapp_cust_moni
											PUBLIC
											e42_xapp
											$<TARGET_OBJECTS:e2_time_obj>
											$<$<STREQUAL:${XAPP_DB},MYSQL_XAPP>:${LIBMYSQLCLIENT_LIBRARIES}>
											-pthread
											-lsctp
											-ldl
											)

# xapp_oran_moni
add_executable(xapp_oran_moni
		xapp_oran_moni.c
								${CMAKE_SOURCE_DIR}/src/util/alg_ds/alg/defer.c
								)

target_link_libraries(xapp_oran_moni
											PUBLIC
											e42_xapp
											$<TARGET_OBJECTS:e2_time_obj>
											$<$<STREQUAL:${XAPP_DB},MYSQL_XAPP>:${LIBMYSQLCLIENT_LIBRARIES}>
											-pthread
											-lsctp
											-ldl
											)
# enable below directive if you need to debug asn code
# target_compile_options(xapp_kpm_moni PUBLIC -DASN_EMIT_DEBUG=1)

option(FALCON_KPM_BROKER "Build the Falcon local KPM application broker" OFF)
set(FALCON_REPO_ROOT "" CACHE PATH "Falcon repository containing broker tests")
if(FALCON_KPM_BROKER)
  if(NOT E2AP_VERSION STREQUAL "E2AP_V3" OR NOT KPM_VERSION STREQUAL "KPM_V3_00" OR NOT XAPP_DB STREQUAL "NONE_XAPP")
    message(FATAL_ERROR "Falcon broker requires E2AP_V3 / KPM_V3_00 / NONE_XAPP")
  endif()
  if(NOT EXISTS "${FALCON_REPO_ROOT}/tests/flexric_kpm_broker_test.c" OR
     NOT EXISTS "${FALCON_REPO_ROOT}/tests/flexric_kpm_broker_fixture.c")
    message(FATAL_ERROR "Set FALCON_REPO_ROOT to the Falcon repository containing tests/flexric_kpm_broker_*.c")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
  add_library(falcon_kpm_broker_core STATIC kpm_broker/kpm_snapshot.c kpm_broker/kpm_protocol.c kpm_broker/kpm_server.c)
  target_include_directories(falcon_kpm_broker_core PUBLIC "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(falcon_kpm_broker_core PUBLIC ASN E2AP_V3 KPM_V3_00 NONE_XAPP)
  target_compile_options(falcon_kpm_broker_core PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(falcon_kpm_broker_core PUBLIC PkgConfig::JSONC pthread)
  add_executable(xapp_kpm_broker xapp_kpm_broker.c kpm_broker/kpm_source.c kpm_broker/kpm_fixture.c)
  target_link_libraries(xapp_kpm_broker PRIVATE falcon_kpm_broker_core e42_xapp $<TARGET_OBJECTS:e2_time_obj> sctp dl m)
  add_executable(falcon_kpm_broker_test "${FALCON_REPO_ROOT}/tests/flexric_kpm_broker_test.c")
  target_link_libraries(falcon_kpm_broker_test PRIVATE falcon_kpm_broker_core)
  target_compile_options(falcon_kpm_broker_test PRIVATE -UNDEBUG)
  # The fixture generator links only KPM ASN. The live executable loads the KPM
  # plugin locally, keeping the incompatible E2AP and KPM runtimes separate.
  add_executable(falcon_kpm_broker_fixture "${FALCON_REPO_ROOT}/tests/flexric_kpm_broker_fixture.c")
  target_include_directories(falcon_kpm_broker_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_compile_options(falcon_kpm_broker_fixture PRIVATE -UNDEBUG)
  target_link_libraries(falcon_kpm_broker_fixture PRIVATE kpm_sm_static)
endif()

# xapp_all_moni
add_executable(xapp_all_moni
		xapp_all_moni.c
								${CMAKE_SOURCE_DIR}/src/util/alg_ds/alg/defer.c
								${CMAKE_SOURCE_DIR}/src/util/e2ap_ngran_type.c
								)

target_link_libraries(xapp_all_moni
											PUBLIC
											e42_xapp
											$<TARGET_OBJECTS:e2_time_obj>
											$<$<STREQUAL:${XAPP_DB},MYSQL_XAPP>:${LIBMYSQLCLIENT_LIBRARIES}>
											-pthread
											-lsctp
											-ldl
											)
