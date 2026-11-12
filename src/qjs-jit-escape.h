/* A temporary literal is scalar-replaced when straight-line, non-throwing
   producers feed only own data definitions and an immediate matching read.
   Skipped producers only read values already owned by the JS frame or pool. */
enum { QJS_BOXED_ESCAPE_MAX_FIELDS = 16 };

static bool qjs_boxed_escape_input(const QJSBoxedInsn *insn)
{
    return insn->falls_through && !insn->branch_target && !insn->pop && insn->push == 1 &&
           (insn->helper == QJS_BOXED_PUSH_INT || insn->helper == QJS_BOXED_PUSH_CONST ||
            insn->helper == QJS_BOXED_PUSH_PRIMITIVE ||
            (insn->helper == QJS_BOXED_SLOT &&
             !(insn->operand & ~(UINT16_MAX | QJS_BOXED_SLOT_ARG | QJS_BOXED_SLOT_REF))));
}

static bool qjs_boxed_escape_mark_targets(const JSFunctionBytecode *b, QJSBoxedWork *work)
{
    unsigned length = b->byte_code_len;
    const uint8_t *code = b->byte_code_buf;
    for (unsigned pc = 0; pc < length; pc = work->insns[pc].next) {
        unsigned op = code[pc];
        if (op >= OP_COUNT || !js_object_layout_flow(op))
            continue;
        unsigned fmt = short_opcode_info(op).fmt;
        unsigned operand = pc + ((fmt == OP_FMT_atom_label_u8 || fmt == OP_FMT_atom_label_u16) ? 5 : 1);
        int64_t target = (int64_t)operand +
                         (fmt == OP_FMT_label8 ? (int8_t)code[operand] :
                          fmt == OP_FMT_label16 ? (int16_t)get_u16(code + operand) :
                          (int32_t)get_u32(code + operand));
        if (target < 0 || target >= length || !work->insns[target].next)
            return false;
        work->insns[target].branch_target = true;
    }
    return true;
}

static const JSObjectLayoutSite *qjs_boxed_escape_layout(const JSFunctionBytecode *b, unsigned pc)
{
    const JSObjectLayoutPlan *plan = b->object_layout;
    if (plan) {
        for (unsigned i = 0; i < plan->site_count; i++) {
            const JSObjectLayoutSite *site = &plan->sites[i];
            if (site->pc == pc && site->first_field <= plan->field_count &&
                site->count <= plan->field_count - site->first_field)
                return site;
        }
    }
    return NULL;
}

/* No prior or subsequent instruction can observe the omitted local write.
   An uncaptured local used only as the immediate return receiver is private. */
static unsigned qjs_boxed_escape_local_read(const JSFunctionBytecode *b, const QJSBoxedInsn *insns,
                                            unsigned start, unsigned pc)
{
    unsigned length = b->byte_code_len;
    if (b->var_ref_count || pc >= length)
        return length;
    const QJSBoxedInsn *put = &insns[pc];
    if (put->helper != QJS_BOXED_SLOT || put->branch_target ||
        (put->operand & ~UINT16_MAX) != QJS_BOXED_SLOT_PUT || put->next >= length)
        return length;

    const QJSBoxedInsn *load = &insns[put->next];
    unsigned slot = put->operand & UINT16_MAX;
    if (start && (insns[0].next != start || insns[0].helper != QJS_BOXED_SLOT ||
                  insns[0].operand != (slot | QJS_BOXED_SLOT_RESET)))
        return length;
    if (load->helper != QJS_BOXED_SLOT || load->branch_target ||
        (load->operand & ~QJS_BOXED_SLOT_TDZ) != slot || load->next >= length)
        return length;

    pc = load->next;
    if (b->byte_code_buf[pc] != OP_get_field || insns[pc].next >= length)
        return length;
    unsigned ret = insns[pc].next;
    if (b->byte_code_buf[ret] != OP_return || insns[ret].next != length)
        return length;
    return pc;
}

static void qjs_boxed_escape_literal(const JSFunctionBytecode *b, QJSBoxedWork *work, unsigned start)
{
    unsigned length = b->byte_code_len;
    const uint8_t *code = b->byte_code_buf;
    QJSBoxedInsn *insns = work->insns;
    const JSObjectLayoutSite *site = NULL;
    const JSObjectLayoutField *fields = NULL;
    if (code[start] == OP_object_layout) {
        site = qjs_boxed_escape_layout(b, start);
        if (!site || !site->count || site->count > QJS_BOXED_ESCAPE_MAX_FIELDS)
            return;
        fields = js_object_layout_fields(b->object_layout) + site->first_field;
    }

    JSAtom atoms[QJS_BOXED_ESCAPE_MAX_FIELDS];
    uint16_t producers[QJS_BOXED_ESCAPE_MAX_FIELDS];
    unsigned count = 0;
    unsigned pc = insns[start].next;
    while (pc < length && qjs_boxed_escape_input(&insns[pc])) {
        if (count == countof(atoms))
            return;
        unsigned field_pc = insns[pc].next;
        if (field_pc >= length || insns[field_pc].branch_target)
            return;
        if (site) {
            if (count >= site->count || code[field_pc] != OP_init_object_field ||
                fields[count].pc != field_pc || get_u32(code + field_pc + 1) != count)
                return;
            atoms[count] = fields[count].atom;
        } else {
            if (code[field_pc] != OP_define_field)
                return;
            atoms[count] = get_u32(code + field_pc + 1);
        }
        if (atoms[count] == JS_ATOM___proto__)
            return;
        producers[count++] = pc;
        pc = insns[field_pc].next;
    }

    if (pc < length && code[pc] != OP_get_field)
        pc = qjs_boxed_escape_local_read(b, insns, start, pc);
    if (pc >= length || code[pc] != OP_get_field || insns[pc].branch_target ||
        (site && count != site->count))
        return;

    JSAtom read = get_u32(code + pc + 1);
    /* Last definition wins for canonical bytecode with repeated keys. */
    while (count) {
        if (atoms[--count] == read) {
            bool branch_target = insns[start].branch_target;
            insns[start] = insns[producers[count]];
            insns[start].branch_target = branch_target;
            insns[start].next = insns[pc].next;
            insns[start].escape_elided = true;
            return;
        }
    }
}

static void qjs_boxed_escape_analyze(const JSFunctionBytecode *b, QJSBoxedWork *work)
{
    unsigned length = b->byte_code_len;
    unsigned pc = 0;
    while (pc < length && b->byte_code_buf[pc] != OP_object &&
           b->byte_code_buf[pc] != OP_object_layout)
        pc = work->insns[pc].next;
    if (pc == length || !qjs_boxed_escape_mark_targets(b, work))
        return;
    for (; pc < length; pc = work->insns[pc].next) {
        if (b->byte_code_buf[pc] == OP_object || b->byte_code_buf[pc] == OP_object_layout)
            qjs_boxed_escape_literal(b, work, pc);
    }
}

static void qjs_boxed_emit_escape_count(QJSArm64Emitter *e, bool layout)
{
    _Static_assert(offsetof(QJSBoxedFrame, ctx) == 0, "boxed ctx load");
    _Static_assert(offsetof(JSContext, rt) % 8 == 0 && offsetof(JSContext, rt) / 8 <= 4095,
                   "boxed runtime load");
    _Static_assert(offsetof(JSRuntime, jit_boxed_escape_literals) % 4 == 0 &&
                   offsetof(JSRuntime, jit_boxed_escape_literals) / 4 <= 4095 &&
                   offsetof(JSRuntime, jit_boxed_escape_layouts) % 4 == 0 &&
                   offsetof(JSRuntime, jit_boxed_escape_layouts) / 4 <= 4095,
                   "boxed escape counter");
    unsigned slot = layout ? offsetof(JSRuntime, jit_boxed_escape_layouts) / 4 :
                             offsetof(JSRuntime, jit_boxed_escape_literals) / 4;
    qjs_arm64_emit(e, 0xf9400270); /* ldr x16, [x19] */
    qjs_arm64_emit(e, 0xf9400210 | ((offsetof(JSContext, rt) / 8) << 10));
    qjs_arm64_emit(e, 0xb9400211 | (slot << 10)); /* ldr w17, [x16, #slot] */
    qjs_arm64_emit(e, 0x11000631); /* add w17, w17, #1 */
    qjs_arm64_emit(e, 0xb9000211 | (slot << 10)); /* str w17, [x16, #slot] */
}
