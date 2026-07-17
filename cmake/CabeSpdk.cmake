# P9M1：仓库内 SPDK v26.01 静态构建的唯一 CMake 接入点。
# 配置期只读取源码、构建产物和构建元数据，不初始化 runtime，也不检查或修改设备。

set(_CABE_SPDK_LOCKED_COMMIT "2ef883ef96e79c3cc16da02f667a7a58c2453f2f")
set(_CABE_SPDK_LOCKED_VERSION "26.01.0")

function(_cabe_spdk_fail message_text)
    message(FATAL_ERROR
        "SPDK preflight failed: ${message_text}\n"
        "Prepare the repository-local dependency with:\n"
        "  ./scripts/setup-spdk.sh init\n"
        "  ./scripts/setup-spdk.sh build --jobs=<N>")
endfunction()

function(_cabe_spdk_is_inside_root output root path)
    file(REAL_PATH "${root}" _root_real)
    file(REAL_PATH "${path}" _path_real)
    if(_path_real STREQUAL _root_real)
        set(${output} TRUE PARENT_SCOPE)
        return()
    endif()
    string(FIND "${_path_real}/" "${_root_real}/" _prefix)
    if(_prefix EQUAL 0)
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_cabe_spdk_pkg_config output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "PKG_CONFIG_PATH=${_cabe_spdk_pkg_paths}"
                "PKG_CONFIG_LIBDIR=${_cabe_spdk_pkg_paths}"
                "PKG_CONFIG_SYSROOT_DIR="
                "${PKG_CONFIG_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _result EQUAL 0)
        _cabe_spdk_fail("repository-local pkg-config command failed: pkg-config ${ARGN}\n${_stderr}")
    endif()
    set(${output} "${_stdout}" PARENT_SCOPE)
endfunction()

function(_cabe_spdk_check_metadata_path module variable root)
    _cabe_spdk_pkg_config(_value --variable=${variable} ${module})
    if(_value STREQUAL "" OR NOT EXISTS "${_value}")
        _cabe_spdk_fail("${module}.pc has missing ${variable}: '${_value}'")
    endif()
    _cabe_spdk_is_inside_root(_inside "${root}" "${_value}")
    if(NOT _inside)
        _cabe_spdk_fail("${module}.pc resolves ${variable} outside current third_party/spdk: '${_value}'")
    endif()
endfunction()

function(_cabe_spdk_stamp_value output stamp key)
    file(STRINGS "${stamp}" _line REGEX "^${key}=" LIMIT_COUNT 1)
    if(NOT _line)
        _cabe_spdk_fail("SPDK Cabe build stamp is missing field '${key}'")
    endif()
    string(REGEX REPLACE "^[^=]+=" "" _value "${_line}")
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(cabe_configure_spdk)
    if(TARGET cabe_spdk_deps)
        return()
    endif()

    find_program(PKG_CONFIG_EXECUTABLE NAMES pkg-config REQUIRED)
    find_program(MAKE_EXECUTABLE NAMES gmake make REQUIRED)
    find_package(Git REQUIRED)

    set(_spdk_root "${PROJECT_SOURCE_DIR}/third_party/spdk")
    if(NOT EXISTS "${_spdk_root}/.git" OR NOT EXISTS "${_spdk_root}/configure")
        _cabe_spdk_fail("third_party/spdk is not initialized")
    endif()
    file(REAL_PATH "${_spdk_root}" _spdk_root)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_spdk_root}" rev-parse HEAD
        RESULT_VARIABLE _git_result
        OUTPUT_VARIABLE _spdk_head
        ERROR_VARIABLE _git_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _git_result EQUAL 0 OR NOT _spdk_head STREQUAL _CABE_SPDK_LOCKED_COMMIT)
        _cabe_spdk_fail("SPDK commit mismatch: got '${_spdk_head}', expected '${_CABE_SPDK_LOCKED_COMMIT}'")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_spdk_root}" submodule status --recursive
        RESULT_VARIABLE _submodule_result
        OUTPUT_VARIABLE _submodule_status
        ERROR_VARIABLE _submodule_error)
    if(NOT _submodule_result EQUAL 0)
        _cabe_spdk_fail("cannot inspect recursive SPDK submodules: ${_submodule_error}")
    endif()
    if(_submodule_status MATCHES "(^|\n)[-+U]")
        _cabe_spdk_fail("SPDK recursive submodules are uninitialized, modified, or at unexpected commits")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_spdk_root}" status --porcelain
                --untracked-files=no --ignore-submodules=none
        RESULT_VARIABLE _status_result
        OUTPUT_VARIABLE _dirty_status
        ERROR_VARIABLE _status_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _status_result EQUAL 0)
        _cabe_spdk_fail("cannot inspect SPDK source worktree: ${_status_error}")
    endif()
    if(NOT _dirty_status STREQUAL "")
        _cabe_spdk_fail("SPDK source tree has tracked modifications; restore the locked submodule before building")
    endif()

    if(NOT EXISTS "${_spdk_root}/VERSION")
        _cabe_spdk_fail("SPDK VERSION file is missing")
    endif()
    file(READ "${_spdk_root}/VERSION" _spdk_version)
    string(STRIP "${_spdk_version}" _spdk_version)
    if(NOT _spdk_version STREQUAL _CABE_SPDK_LOCKED_VERSION)
        _cabe_spdk_fail("SPDK version mismatch: got '${_spdk_version}', expected '${_CABE_SPDK_LOCKED_VERSION}'")
    endif()

    set(_config_mk "${_spdk_root}/mk/config.mk")
    set(_config_header "${_spdk_root}/build/include/spdk/config.h")
    set(_build_stamp "${_spdk_root}/build/.cabe-build-stamp")
    if(NOT EXISTS "${_config_mk}" OR NOT EXISTS "${_config_header}")
        _cabe_spdk_fail("SPDK has not been configured and built")
    endif()
    if(NOT EXISTS "${_build_stamp}")
        _cabe_spdk_fail("SPDK Cabe build stamp is missing; rebuild with ./scripts/setup-spdk.sh build --jobs=<N>")
    endif()

    _cabe_spdk_stamp_value(_stamp_schema "${_build_stamp}" schema)
    _cabe_spdk_stamp_value(_stamp_commit "${_build_stamp}" commit)
    _cabe_spdk_stamp_value(_stamp_version "${_build_stamp}" version)
    _cabe_spdk_stamp_value(_stamp_config_hash "${_build_stamp}" config_sha256)
    _cabe_spdk_stamp_value(_stamp_header_hash "${_build_stamp}" config_header_sha256)
    file(SHA256 "${_config_mk}" _config_hash)
    file(SHA256 "${_config_header}" _config_header_hash)
    if(NOT "${_stamp_schema}" STREQUAL "1" OR
       NOT "${_stamp_commit}" STREQUAL "${_CABE_SPDK_LOCKED_COMMIT}" OR
       NOT "${_stamp_version}" STREQUAL "${_CABE_SPDK_LOCKED_VERSION}" OR
       NOT "${_stamp_config_hash}" STREQUAL "${_config_hash}" OR
       NOT "${_stamp_header_hash}" STREQUAL "${_config_header_hash}")
        _cabe_spdk_fail("SPDK Cabe build stamp does not match the locked commit or current build configuration")
    endif()
    file(STRINGS "${_config_mk}" _shared_lines
         REGEX "^[ \t]*CONFIG_SHARED[ \t]*[?:+]?=")
    if(NOT _shared_lines OR NOT _shared_lines MATCHES "=[ \t]*n([ \t]*$|;)")
        _cabe_spdk_fail("SPDK must use its default static mode (CONFIG_SHARED=n)")
    endif()
    file(READ "${_config_header}" _config_header_text)
    if(_config_header_text MATCHES "#[ \t]*define[ \t]+SPDK_CONFIG_SHARED[ \t]+1")
        _cabe_spdk_fail("SPDK generated config enables shared libraries; rebuild in static mode")
    endif()

    set(_spdk_pc_dir "${_spdk_root}/build/lib/pkgconfig")
    set(_dpdk_pc_dir "${_spdk_root}/dpdk/build/lib/pkgconfig")
    set(_spdk_lib_dir "${_spdk_root}/build/lib")
    set(_dpdk_lib_dir "${_spdk_root}/dpdk/build/lib")
    foreach(_required_dir IN ITEMS
            "${_spdk_pc_dir}" "${_dpdk_pc_dir}"
            "${_spdk_lib_dir}" "${_dpdk_lib_dir}")
        if(NOT IS_DIRECTORY "${_required_dir}")
            _cabe_spdk_fail("required SPDK build directory is missing: ${_required_dir}")
        endif()
    endforeach()

    set(_required_pc
        "${_spdk_pc_dir}/spdk_nvme.pc"
        "${_spdk_pc_dir}/spdk_env_dpdk.pc"
        "${_dpdk_pc_dir}/libdpdk.pc")
    foreach(_pc IN LISTS _required_pc)
        if(NOT EXISTS "${_pc}")
            _cabe_spdk_fail("required pkg-config metadata is missing: ${_pc}")
        endif()
    endforeach()

    # Deliberately set both variables: an empty or system PKG_CONFIG_PATH must never
    # make a repository-local SPDK build silently consume a system installation.
    set(_cabe_spdk_pkg_paths "${_spdk_pc_dir}:${_dpdk_pc_dir}")
    _cabe_spdk_pkg_config(_exists --exists spdk_nvme spdk_env_dpdk libdpdk)

    foreach(_module IN ITEMS spdk_nvme spdk_env_dpdk libdpdk)
        _cabe_spdk_check_metadata_path(${_module} pcfiledir "${_spdk_root}")
    endforeach()
    # SPDK's generated .pc files carry concrete Cflags/Libs paths but do not define
    # includedir/libdir variables; DPDK defines both. The token pass below validates
    # every concrete SPDK path, while these checks cover DPDK's variables as well.
    _cabe_spdk_check_metadata_path(libdpdk includedir "${_spdk_root}")
    _cabe_spdk_check_metadata_path(libdpdk libdir "${_spdk_root}")

    _cabe_spdk_pkg_config(_spdk_cflags --cflags-only-I spdk_nvme spdk_env_dpdk)
    _cabe_spdk_pkg_config(_dpdk_cflags --cflags-only-I libdpdk)
    separate_arguments(_include_tokens UNIX_COMMAND "${_spdk_cflags} ${_dpdk_cflags}")
    set(_include_dirs)
    foreach(_token IN LISTS _include_tokens)
        if(NOT _token MATCHES "^-I(.+)$")
            continue()
        endif()
        set(_include_dir "${CMAKE_MATCH_1}")
        if(NOT IS_DIRECTORY "${_include_dir}")
            _cabe_spdk_fail("pkg-config include directory does not exist: ${_include_dir}")
        endif()
        _cabe_spdk_is_inside_root(_inside "${_spdk_root}" "${_include_dir}")
        if(NOT _inside)
            _cabe_spdk_fail("pkg-config selected an include directory outside current third_party/spdk: ${_include_dir}")
        endif()
        list(APPEND _include_dirs "${_include_dir}")
    endforeach()

    _cabe_spdk_pkg_config(_spdk_lib_flags --static --libs spdk_nvme spdk_env_dpdk)
    _cabe_spdk_pkg_config(_dpdk_lib_flags --static --libs libdpdk)
    execute_process(
        COMMAND "${MAKE_EXECUTABLE}" -s -C "${_spdk_root}" .libs_only_other
        RESULT_VARIABLE _other_result
        OUTPUT_VARIABLE _other_lib_flags
        ERROR_VARIABLE _other_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _other_result EQUAL 0)
        _cabe_spdk_fail("SPDK build metadata target .libs_only_other failed: ${_other_error}")
    endif()

    separate_arguments(_link_tokens UNIX_COMMAND
        "${_spdk_lib_flags} ${_dpdk_lib_flags} ${_other_lib_flags}")

    # First collect all local search paths so later -l entries can be resolved to
    # concrete archives irrespective of token ordering.
    set(_local_library_dirs "${_spdk_lib_dir}" "${_dpdk_lib_dir}")
    set(_system_library_dirs)
    foreach(_token IN LISTS _link_tokens)
        if(NOT _token MATCHES "^-L(.+)$")
            continue()
        endif()
        set(_library_dir "${CMAKE_MATCH_1}")
        if(NOT IS_DIRECTORY "${_library_dir}")
            _cabe_spdk_fail("pkg-config library directory does not exist: ${_library_dir}")
        endif()
        _cabe_spdk_is_inside_root(_inside "${_spdk_root}" "${_library_dir}")
        if(_inside)
            list(APPEND _local_library_dirs "${_library_dir}")
        elseif(_library_dir MATCHES "^/usr(/|$)" OR _library_dir MATCHES "^/lib(32|64)?(/|$)")
            list(APPEND _system_library_dirs "${_library_dir}")
        else()
            _cabe_spdk_fail("pkg-config selected an external or stale library directory: ${_library_dir}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _local_library_dirs)
    list(REMOVE_DUPLICATES _system_library_dirs)

    set(_whole_archives)
    set(_ordinary_archives)
    set(_system_libraries)
    set(_link_options)
    foreach(_token IN LISTS _link_tokens)
        if(_token MATCHES "^-L")
            continue()
        endif()
        if(_token STREQUAL "-Wl,--whole-archive" OR
           _token STREQUAL "-Wl,--no-whole-archive" OR
           _token STREQUAL "-Wl,--start-group" OR
           _token STREQUAL "-Wl,--end-group")
            continue()
        endif()

        set(_archive "")
        if(IS_ABSOLUTE "${_token}" AND _token MATCHES "\\.a$")
            if(NOT EXISTS "${_token}")
                _cabe_spdk_fail("static dependency archive is missing: ${_token}")
            endif()
            _cabe_spdk_is_inside_root(_inside "${_spdk_root}" "${_token}")
            if(NOT _inside)
                _cabe_spdk_fail("static dependency archive is outside current third_party/spdk: ${_token}")
            endif()
            file(REAL_PATH "${_token}" _archive)
        elseif(_token MATCHES "^-l:(.+\\.a)$")
            set(_archive_name_from_flag "${CMAKE_MATCH_1}")
            foreach(_directory IN LISTS _local_library_dirs)
                if(EXISTS "${_directory}/${_archive_name_from_flag}")
                    file(REAL_PATH "${_directory}/${_archive_name_from_flag}" _archive)
                    break()
                endif()
            endforeach()
            if(_archive STREQUAL "")
                _cabe_spdk_fail("pkg-config named a static archive outside the local closure: ${_token}")
            endif()
        elseif(_token MATCHES "^-l(.+)$")
            set(_library_name "${CMAKE_MATCH_1}")
            foreach(_directory IN LISTS _local_library_dirs)
                if(EXISTS "${_directory}/lib${_library_name}.a")
                    file(REAL_PATH "${_directory}/lib${_library_name}.a" _archive)
                    break()
                endif()
            endforeach()
            if(_archive STREQUAL "")
                if(_library_name STREQUAL "spdk" OR
                   _library_name MATCHES "^spdk_" OR
                   _library_name MATCHES "^rte_")
                    _cabe_spdk_fail(
                        "required repository-local SPDK/DPDK archive is missing: lib${_library_name}.a")
                endif()
                list(APPEND _system_libraries "${_library_name}")
                continue()
            endif()
        elseif(_token MATCHES "^-Wl," OR _token STREQUAL "-pthread")
            list(APPEND _link_options "${_token}")
            continue()
        elseif(NOT _token STREQUAL "")
            list(APPEND _system_libraries "${_token}")
            continue()
        endif()

        get_filename_component(_archive_name "${_archive}" NAME)
        if(_archive_name MATCHES "^libspdk_.*\\.a$" OR
           _archive_name MATCHES "^librte_.*\\.a$")
            list(APPEND _whole_archives "${_archive}")
        else()
            list(APPEND _ordinary_archives "${_archive}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES _include_dirs)
    list(REMOVE_DUPLICATES _whole_archives)
    list(REMOVE_DUPLICATES _ordinary_archives)
    list(REMOVE_DUPLICATES _system_libraries)
    list(REMOVE_DUPLICATES _link_options)

    foreach(_critical IN ITEMS
            "${_spdk_lib_dir}/libspdk_nvme.a"
            "${_spdk_lib_dir}/libspdk_env_dpdk.a"
            "${_dpdk_lib_dir}/librte_eal.a")
        file(REAL_PATH "${_critical}" _critical_real)
        if(NOT EXISTS "${_critical}" OR NOT _critical_real IN_LIST _whole_archives)
            _cabe_spdk_fail("critical static archive is missing from the resolved dependency closure: ${_critical}")
        endif()
    endforeach()

    if(NOT _whole_archives)
        _cabe_spdk_fail("no SPDK/DPDK static archives were resolved")
    endif()

    file(TIMESTAMP "${_build_stamp}" _stamp_epoch "%s" UTC)
    foreach(_archive IN LISTS _whole_archives _ordinary_archives)
        file(TIMESTAMP "${_archive}" _archive_epoch "%s" UTC)
        if(_archive_epoch GREATER _stamp_epoch)
            _cabe_spdk_fail("SPDK archive changed after the verified build stamp: ${_archive}")
        endif()
    endforeach()

    add_library(cabe_spdk_deps INTERFACE)
    add_library(cabe::spdk_deps ALIAS cabe_spdk_deps)
    target_include_directories(cabe_spdk_deps SYSTEM INTERFACE ${_include_dirs})
    if(_system_library_dirs)
        target_link_directories(cabe_spdk_deps INTERFACE ${_system_library_dirs})
    endif()
    string(JOIN "," _whole_archive_arguments ${_whole_archives})
    target_link_libraries(cabe_spdk_deps INTERFACE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_whole_archive_arguments}>"
        ${_ordinary_archives}
        ${_system_libraries})
    if(_link_options)
        target_link_options(cabe_spdk_deps INTERFACE ${_link_options})
    endif()

    list(LENGTH _whole_archives _whole_archive_count)
    message(STATUS
        "Cabe SPDK dependency closure: v${_spdk_version}, "
        "${_whole_archive_count} SPDK/DPDK static archives")
endfunction()
