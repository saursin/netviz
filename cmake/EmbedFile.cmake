# Embeds a binary file (font, image, ...) into the program as a byte array.
#
#   netviz_embed_file(<target> <input-file> <symbol>)
#
# Adds a generated source to <target> that defines, in the global namespace:
#   extern const unsigned char <symbol>[];
#   extern const std::size_t   <symbol>_size;
# The source is regenerated at build time whenever <input-file> changes.

# When run as a script (cmake -P), generate the source file.
if(CMAKE_SCRIPT_MODE_FILE)
    file(READ "${INPUT}" hex HEX)
    string(LENGTH "${hex}" hex_length)
    math(EXPR size "${hex_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE "${OUTPUT}"
        "// Generated from ${INPUT}. Do not edit.\n"
        "#include <cstddef>\n\n"
        "extern const unsigned char ${SYMBOL}[] = {${bytes}};\n"
        "extern const std::size_t ${SYMBOL}_size = ${size};\n")
    return()
endif()

set(NETVIZ_EMBED_SCRIPT ${CMAKE_CURRENT_LIST_FILE})

function(netviz_embed_file target input symbol)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/embed/${symbol}.cc)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${output} -DSYMBOL=${symbol}
                -P ${NETVIZ_EMBED_SCRIPT}
        DEPENDS ${input} ${NETVIZ_EMBED_SCRIPT}
        COMMENT "Embedding ${input}"
        VERBATIM)
    target_sources(${target} PRIVATE ${output})
endfunction()
