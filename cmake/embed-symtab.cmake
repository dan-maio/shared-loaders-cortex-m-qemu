# embed-symtab.cmake — Generate and embed a symbol table from object files.
#
# Generates symtab.c from all object files BEFORE linking, avoiding circular deps.
#
# Usage: include(embed-symtab) && embed_symtab(target_name)

function(embed_symtab target)
    set(SYMTAB_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/gen-symtab-objs.py")
    set(SYMTAB_DIR "${CMAKE_CURRENT_BINARY_DIR}/symtab")
    set(SYMTAB_C "${SYMTAB_DIR}/symtab.c")
    set(SYMTAB_OBJ "${SYMTAB_DIR}/symtab.o")

    find_program(PYTHON3 python3)
    if(NOT PYTHON3)
        message(WARNING "embed_symtab: python3 not found")
        return()
    endif()

    # Get all source files for the target
    get_target_property(SOURCES ${target} SOURCES)

    # Collect object file paths
    set(OBJ_FILES)
    foreach(src ${SOURCES})
        if(src MATCHES "\\.(c|cpp|cc|cxx)$")
            # Compute object file path
            get_filename_component(src_abs "${src}" ABSOLUTE)
            file(RELATIVE_PATH src_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${src_abs}")
            string(REGEX REPLACE "\\.[^.]+$" ".o" obj_rel "${src_rel}")
            list(APPEND OBJ_FILES "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.dir/${obj_rel}")
        endif()
    endforeach()

    # Add symtab.c as a source (will be generated before compile)
    set_source_files_properties("${SYMTAB_OBJ}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

    # Generate symtab.c from object files (runs before link)
    add_custom_command(
        OUTPUT "${SYMTAB_C}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SYMTAB_DIR}"
        COMMAND ${PYTHON3} "${SYMTAB_SCRIPT}" "${SYMTAB_C}" ${OBJ_FILES}
        DEPENDS ${OBJ_FILES}
        COMMENT "Generating embedded symbol table"
        VERBATIM
    )

    # Compile symtab.c to symtab.o
    add_custom_command(
        OUTPUT "${SYMTAB_OBJ}"
        COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m4 -mthumb -c "${SYMTAB_C}" -o "${SYMTAB_OBJ}"
        DEPENDS "${SYMTAB_C}"
        COMMENT "Compiling embedded symbol table"
        VERBATIM
    )

    # Link symtab.o into the firmware
    target_sources(${target} PRIVATE "${SYMTAB_OBJ}")
endfunction()
