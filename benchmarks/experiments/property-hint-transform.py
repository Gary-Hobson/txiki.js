"""Create the isolated, validated own-property slot-hint experiment.

Only OP_get_field and OP_get_field2 use the cache, only for the original
receiver. Prototype traversal and every existing property-flags check remain
unchanged. This module does not read, write, build, or execute QuickJS itself.
"""


def _replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise ValueError(f"{label}: expected one source anchor, found {count}")
    return source.replace(old, new, 1)


def transform(source: str) -> str:
    """Return QuickJS source with a 1024-entry runtime atom-indexed slot hint.

    JS_NewRuntime2 calloc-initializes the runtime, including these hints. Each
    hint stores slot+1, never an object, shape, atom, or property pointer. A
    hit validates its bounds and the current shape property's atom, and the
    opcode still checks current descriptor flags. Consequently collisions,
    shape mutation/compaction, GC and allocator/atom reuse need no invalidation.

    This transform deliberately rejects already-transformed or drifted input.
    """
    if "own_property_slot_hints" in source:
        raise ValueError("property hints are already present in the input")

    if source.count("rt = mf->js_calloc(opaque, 1, sizeof(JSRuntime));") != 1:
        raise ValueError("cannot verify zero initialization of JSRuntime")

    source = _replace_once(
        source,
        "    /* Shape hash table */\n",
        "    /* Atom-indexed own-property slot hints; zero means no hint.\n"
        "       Entries own no GC references and are validated on every use. */\n"
        "    uint32_t own_property_slot_hints[1024];\n\n"
        "    /* Shape hash table */\n",
        "runtime hint storage",
    )

    helper = r'''/* A slot hint is safe across shape mutation and address reuse because
   its bounds and atom are checked against the object's current shape.
   Descriptor flags are deliberately left to the caller's existing checks. */
static inline JSShapeProperty *find_own_property_hint(JSRuntime *rt,
                                                      JSProperty **ppr,
                                                      JSObject *p, JSAtom atom)
{
    JSShape *sh = p->shape;
    JSShapeProperty *prs;
    uint32_t *hint = &rt->own_property_slot_hints[atom & 1023];
    uint32_t h = *hint;

    if (h && h <= (uint32_t)sh->prop_count) {
        prs = &get_shape_prop(sh)[h - 1];
        if (prs->atom == atom) {
            *ppr = &p->prop[h - 1];
            return prs;
        }
    }

    prs = find_own_property(ppr, p, atom);
    if (prs)
        *hint = (uint32_t)(*ppr - p->prop) + 1;
    return prs;
}

'''
    source = _replace_once(
        source,
        "/* Release a counted reference an open var_ref held on the coroutine\n",
        helper
        + "/* Release a counted reference an open var_ref held on the coroutine\n",
        "hint helper after find_own_property",
    )

    for opcode, next_opcode in (
        ("OP_get_field", "OP_get_field2"),
        ("OP_get_field2", "OP_put_field"),
    ):
        start_anchor = f"        CASE({opcode}):\n"
        end_anchor = f"        CASE({next_opcode}):\n"
        if source.count(start_anchor) != 1 or source.count(end_anchor) != 1:
            raise ValueError(f"cannot uniquely locate {opcode} handler")
        start = source.index(start_anchor)
        end = source.index(end_anchor, start + len(start_anchor))
        handler = source[start:end]

        handler = _replace_once(
            handler,
            "                    p = JS_VALUE_GET_OBJ(obj);\n"
            "                    for(;;) {\n"
            "                        prs = find_own_property(&pr, p, atom);\n",
            "                    p = JS_VALUE_GET_OBJ(obj);\n"
            "                    prs = find_own_property_hint(rt, &pr, p, atom);\n"
            "                    for(;;) {\n",
            f"{opcode} initial receiver lookup",
        )
        handler = _replace_once(
            handler,
            "                        p = p->shape->proto;\n"
            "                        if (!p) {\n"
            "                            val = JS_UNDEFINED;\n"
            "                            break;\n"
            "                        }\n",
            "                        p = p->shape->proto;\n"
            "                        if (!p) {\n"
            "                            val = JS_UNDEFINED;\n"
            "                            break;\n"
            "                        }\n"
            "                        prs = find_own_property(&pr, p, atom);\n",
            f"{opcode} uncached prototype lookup",
        )
        source = source[:start] + handler + source[end:]

    return source
