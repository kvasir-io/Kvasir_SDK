# A stand-in for CMake's FetchContent (tests/locate/run.cmake puts this directory first on CMAKE_MODULE_PATH): records
# every call in ${LOCATE_FETCH_LOG} and makes <name>_SOURCE_DIR a directory under ${LOCATE_FETCH_DIR} that holds the
# marker files kvasir_locate checks - nothing is downloaded.
function(FetchContent_Declare name)
    string(REPLACE ";" " " _args "${ARGN}")
    file(APPEND "${LOCATE_FETCH_LOG}" "declare ${name} ${_args}\n")
    # an empty GIT_SUBMODULES value is lost in ARGN's spelling above: say it
    list(FIND ARGN GIT_SUBMODULES _i)
    if(_i GREATER -1)
        math(EXPR _v "${_i} + 1")
        list(LENGTH ARGN _n)
        if(_v LESS _n)
            list(GET ARGN ${_v} _value)
        else()
            set(_value "")
        endif()
        file(APPEND "${LOCATE_FETCH_LOG}" "submodules ${name} [${_value}]\n")
    endif()
endfunction()

function(FetchContent_MakeAvailable)
    foreach(name IN LISTS ARGN)
        file(APPEND "${LOCATE_FETCH_LOG}" "make_available ${name}\n")
        string(TOLOWER "${name}" _lower)
        set(_dir "${LOCATE_FETCH_DIR}/${_lower}-src")
        file(WRITE "${_dir}/cmake/chip.cmake" "")
        file(WRITE "${_dir}/CMakeLists.txt" "")
        set(${_lower}_SOURCE_DIR
            "${_dir}"
            PARENT_SCOPE)
    endforeach()
endfunction()
