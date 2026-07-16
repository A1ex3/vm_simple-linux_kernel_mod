#!/usr/bin/env python3

"""
VM Assembler Documentation
--------------------------
Instruction Format: [Opcode (1b)][Arg0 (1b)][Arg1 (1b)][Arg2 (1b)][Immediate (8b)]
Registers: r0-r6, sp (r7)

Instruction Set (Mnemonics & Syntax):
- Arithmetic (r, r, r):
    add, sub, mul, div
- Data Movement:
    mov  r, r      (Register to Register)
    mov  r, imm    (Immediate to Register)
    ss   r, imm    (Store to Stack: mem[imm] = r)
    ls   r, imm    (Load from Stack: r = mem[imm])
    len  r, imm    (Load string length)
- Control Flow:
    jmp  imm
    jeq, jne, jlt, jle, jgt, jge  r, r, imm
- System:
    exit           (Terminate)
    exit imm       (Terminate with status code)
    call imm       (Exec function: 0:printint, 1:push_rb, 2:pop_rb)

Directives:
- .equ <name> <val>       : Integer constant definition
- .macro <name> <params>  : Start macro block
- .endmacro               : End macro block
- <name>: .alloc <n>      : Allocate n * 8 bytes
- <name>: .string <"s">   : Allocate & init memory with string

Constraints & Validation:
- Register SP (r7): Read-only for arithmetic, mov, ls, and call operations.
- Stack Operations (ss/ls):
    - Offset must be 8-byte aligned (offset % 8 == 0).
    - Offset must be within bounds [0, 504].
- Jump Instructions:
    - Immediate (imm) cannot be 0 (prevents infinite loop).
    - Target must be within program bounds.
- Function Calls:
    - Immediate must be in range [0, 2].
"""

import sys
import struct
import re
import ast
from dataclasses import dataclass

VM_OPC_ADD = 1
VM_OPC_SUB = 2
VM_OPC_MUL = 3
VM_OPC_DIVIDE = 4
VM_OPC_MOV = 5
VM_OPC_MOV_REG = 6
VM_OPC_JMP = 7
VM_OPC_JEQ = 8
VM_OPC_JNE = 9
VM_OPC_JLT = 10
VM_OPC_JLE = 11
VM_OPC_JGT = 12
VM_OPC_JGE = 13
VM_OPC_EXIT = 14
VM_OPC_EXIT_STATUS_CODE = 15
VM_OPC_EXEC_FUNCTION = 16
VM_OPC_STORE_STACK = 17
VM_OPC_LOAD_STACK = 18
VM_OPC_GET_LEN = 19

VM_REGSP = 7
VM_FUNCTIONS_COUNT = 3
FUNCTIONS = {"printint": 0, "push_rb": 1, "pop_rb": 2}

@dataclass
class OpVariant:
    op_code: int
    pattern: str

REG0 = r"(?P<arg0>r[0-6]|sp)"
REG1 = r"(?P<arg1>r[0-6]|sp)"
REG2 = r"(?P<arg2>r[0-6]|sp)"
IMM  = r"(?P<imm>-?(?:0x[0-9a-fA-F]+|\d+)|[a-zA-Z_]\w*)"

OPCODES = {
    "add": [OpVariant(VM_OPC_ADD, f"^{REG0}\\s+{REG1}\\s+{REG2}$")],
    "sub": [OpVariant(VM_OPC_SUB, f"^{REG0}\\s+{REG1}\\s+{REG2}$")],
    "mul": [OpVariant(VM_OPC_MUL, f"^{REG0}\\s+{REG1}\\s+{REG2}$")],
    "div": [OpVariant(VM_OPC_DIVIDE, f"^{REG0}\\s+{REG1}\\s+{REG2}$")],
    "mov": [OpVariant(VM_OPC_MOV_REG, f"^{REG0}\\s+{REG1}$"), OpVariant(VM_OPC_MOV, f"^{REG0}\\s+{IMM}$")],
    "jmp": [OpVariant(VM_OPC_JMP, f"^{IMM}$")],
    "jeq": [OpVariant(VM_OPC_JEQ, f"^{REG0}\\s+{REG1}\\s+{IMM}$")],
    "jne": [OpVariant(VM_OPC_JNE, f"^{REG0}\\s+{REG1}\\s+{IMM}$")],
    "jlt": [OpVariant(VM_OPC_JLT, f"^{REG0}\\s+{REG1}\\s+{IMM}$")],
    "jle": [OpVariant(VM_OPC_JLE, f"^{REG0}\\s+{REG1}\\s+{IMM}$")],
    "jgt": [OpVariant(VM_OPC_JGT, f"^{REG0}\\s+{REG1}\\s+{IMM}$")],
    "jge": [OpVariant(VM_OPC_JGE, f"^{REG0}\\s+{REG1}\\s+{IMM}$")],
    "exit": [OpVariant(VM_OPC_EXIT, r"^$"), OpVariant(VM_OPC_EXIT_STATUS_CODE, f"^{IMM}$")],
    "call": [OpVariant(VM_OPC_EXEC_FUNCTION, f"^{IMM}$")],
    "ss": [OpVariant(VM_OPC_STORE_STACK, f"^{REG0}\\s+{IMM}$")],
    "ls": [OpVariant(VM_OPC_LOAD_STACK, f"^{REG0}\\s+{IMM}$")],
    "len": [OpVariant(VM_OPC_GET_LEN, f"^{REG0}\\s+{IMM}$")],
}

def parse_reg(reg_str: str) -> int:
    if not reg_str: return 0
    return VM_REGSP if reg_str.lower() == "sp" else int(reg_str.lower()[1])

def verify_instruction(op_code: int, a0: int, a1: int, a2: int, imm: int, i: int, instr_count: int):
    if op_code > VM_OPC_LOAD_STACK or op_code == 0:
        raise SyntaxError(f"VERIFI_ERROR_INCORRECT_OP_CODE ({op_code})")
    if not (0 <= a0 <= 7) or not (0 <= a1 <= 7) or not (0 <= a2 <= 7):
        raise SyntaxError(f"VERIFI_ERROR_REGISTER at instr {i}")
    if a0 == VM_REGSP and op_code in (VM_OPC_ADD, VM_OPC_SUB, VM_OPC_MUL, VM_OPC_DIVIDE, VM_OPC_MOV, VM_OPC_MOV_REG, VM_OPC_LOAD_STACK, VM_OPC_EXEC_FUNCTION):
        raise SyntaxError(f"VERIFI_ERROR_READ_ONLY_REGISTER (Write to SP at instr {i})")
    if op_code in (VM_OPC_STORE_STACK, VM_OPC_LOAD_STACK):
        if imm % 8 != 0:
            raise SyntaxError(f"VERIFI_ERROR_STACK_ALIGNMENT (Offset {imm} not aligned by 8 at instr {i})")
        if imm < 0 or imm > 504:
            raise SyntaxError(f"VERIFI_ERROR_STACK_OUT_OF_BOUNDS (Offset {imm} at instr {i})")
    elif VM_OPC_JMP <= op_code <= VM_OPC_JGE:
        if imm == 0: raise SyntaxError(f"VERIFI_ERROR_INFINITE_LOOP (Jump to itself at instr {i})")
        if not (0 <= (i + imm) < instr_count): raise SyntaxError(f"VERIFI_ERROR_OUT_OF_PROGRAMM (Jump to {i + imm} from {i})")
    elif op_code == VM_OPC_EXEC_FUNCTION:
        if not (0 <= imm < VM_FUNCTIONS_COUNT): raise SyntaxError(f"VERIFI_ERROR_FUNCTION_DOESNT_EXISTS (ID {imm} at instr {i})")

def phase0_macros_and_equ(source_code: str):
    lines = source_code.splitlines()
    expanded_lines = []
    constants = {}
    macros = {}
    
    in_macro = False
    macro_name, macro_params, macro_body = "", [], []

    for line_idx, line in enumerate(lines):
        line = re.sub(r'[#;].*$', '', line).strip()
        if not line: continue

        equ_match = re.match(r'^\.equ\s+([a-zA-Z_]\w*)\s+(.+)$', line)
        if equ_match and not in_macro:
            name, val_str = equ_match.groups()
            try:
                constants[name] = int(val_str, 0)
            except ValueError:
                raise SyntaxError(f"Invalid .equ value '{val_str}' at line {line_idx+1}")
            continue

        macro_match = re.match(r'^\.macro\s+([a-zA-Z_]\w*)(.*)$', line)
        if macro_match:
            in_macro = True
            macro_name = macro_match.group(1)
            macro_params = macro_match.group(2).strip().split()
            macro_body = []
            continue

        if in_macro:
            if line == ".endmacro":
                macros[macro_name] = (macro_params, macro_body)
                in_macro = False
            else:
                macro_body.append(line)
            continue

        parts = line.split(maxsplit=1)
        mnemonic = parts[0]
        
        if mnemonic in macros:
            args_str = parts[1] if len(parts) > 1 else ""
            args = [a.strip() for a in args_str.replace(',', ' ').split()]
            m_params, m_body = macros[mnemonic]

            if len(args) != len(m_params):
                raise SyntaxError(f"Macro '{mnemonic}' expects {len(m_params)} args, got {len(args)}")

            for m_line in m_body:
                exp_line = m_line
                for param, arg in zip(m_params, args):
                    exp_line = exp_line.replace(param, arg)
                expanded_lines.append(exp_line)
        else:
            expanded_lines.append(line)

    if in_macro:
        raise SyntaxError("Unclosed .macro directive found")

    return expanded_lines, constants

def phase1_alloc_and_string(expanded_lines: list, data_labels: dict):
    processed_lines = []
    stack_ptr = 0

    for line_idx, line in enumerate(expanded_lines):
        alloc_match = re.match(r'^([a-zA-Z_]\w*)\s*:\s*\.alloc\s+(\d+)$', line)
        if alloc_match:
            name, slots = alloc_match.groups()
            data_labels[name] = stack_ptr
            stack_ptr += int(slots) * 8
            continue
            
        string_match = re.match(r'^([a-zA-Z_]\w*)\s*:\s*\.string\s+(.+)$', line)
        if string_match:
            name, raw_str = string_match.groups()
            byte_data = ast.literal_eval(raw_str).encode('utf-8')
            data_labels[name], data_labels[f"{name}_len"] = stack_ptr, len(byte_data)
            chunks = [byte_data[i:i+4] for i in range(0, len(byte_data), 4)]
            
            for chunk in chunks:
                chunk += b'\x00' * (4 - len(chunk))
                val = struct.unpack('<i', chunk)[0]
                processed_lines.append(f"mov r6, {val}")
                processed_lines.append(f"ss r6, {stack_ptr}")
                stack_ptr += 8
            continue
            
        processed_lines.append(line)
    return processed_lines, data_labels

def compile_asm(source_code: str) -> bytes:
    expanded_lines, data_labels = phase0_macros_and_equ(source_code)
    
    processed_lines, data_labels = phase1_alloc_and_string(expanded_lines, data_labels)
    
    instructions_text = []
    code_labels = {}
    current_addr = 0
    for line in processed_lines:
        if line.endswith(':'):
            code_labels[line[:-1].strip()] = current_addr
        elif ':' in line:
            label, inst = line.split(':', 1)
            code_labels[label.strip()] = current_addr
            if inst.strip():
                instructions_text.append((current_addr, inst.strip()))
                current_addr += 1
        else:
            instructions_text.append((current_addr, line))
            current_addr += 1

    bytecode = bytearray()
    for i, inst_text in instructions_text:
        parts = inst_text.replace(',', ' ').split()
        if not parts: continue
        
        mnemonic = parts[0].lower()
        args_str = " ".join(parts[1:]) 
        
        if mnemonic not in OPCODES:
            raise SyntaxError(f"Unknown instruction '{mnemonic}' -> {inst_text}")

        matched = False
        for variant in OPCODES[mnemonic]:
            match = re.match(variant.pattern, args_str)
            if match:
                extracted = match.groupdict()
                op = variant.op_code
                a0, a1, a2 = parse_reg(extracted.get("arg0")), parse_reg(extracted.get("arg1")), parse_reg(extracted.get("arg2"))
                
                imm_str = extracted.get("imm")
                imm = 0
                if imm_str:
                    if op == VM_OPC_GET_LEN:
                        len_label = imm_str + "_len"
                        if len_label in data_labels:
                            imm = data_labels[len_label]
                        else:
                            raise ValueError(f"Label '{imm_str}' has no length (is it a string?)")
                    
                    elif imm_str in data_labels:
                        imm = data_labels[imm_str]
                    elif imm_str in code_labels:
                        imm = code_labels[imm_str] - i if VM_OPC_JMP <= op <= VM_OPC_JGE else code_labels[imm_str]
                    elif imm_str.lower() in FUNCTIONS:
                        imm = FUNCTIONS[imm_str.lower()]
                    else:
                        try: 
                            imm = int(imm_str, 0)
                        except ValueError: 
                            raise ValueError(f"Unknown label or value: '{imm_str}'")

                # `len` has no real VM opcode: the VM only understands VM_OPC_MOV.
                # Once the string length has been resolved into `imm` above, emit
                # a plain "load immediate into register" instruction instead.
                if op == VM_OPC_GET_LEN:
                    op = VM_OPC_MOV

                verify_instruction(op, a0, a1, a2, imm, i, len(instructions_text))
                bytecode.extend(struct.pack('<BBBBq', op, a0, a1, a2, imm))
                matched = True
                break
                
        if not matched: 
            raise SyntaxError(f"Invalid syntax -> '{inst_text}'")

    return bytecode

def print_beautiful_bytecode(bytecode: bytearray):
    instr_size = 12  # op (1) + arg0 (1) + arg1 (1) + arg2 (1) + imm (8)
    print("\n--------------------------BYTECODE DUMP----------------------------------------------")
    print(f"{'Addr':<6} | {'Raw Hex Data':<35} | {'Decoded Struct (op, a0, a1, a2, imm)':<45}")
    print("-" * 85)
    for i in range(0, len(bytecode), instr_size):
        chunk = bytecode[i:i+instr_size]
        if len(chunk) < instr_size:
            break
        hex_str = ' '.join(f'{b:02x}' for b in chunk)
        imm_val = struct.unpack('<q', chunk[4:12])[0]
        print(f"[{i//instr_size:04x}] | {hex_str:<35} | op:{chunk[0]:<2} a0:{chunk[1]} a1:{chunk[2]} a2:{chunk[3]} | imm:{imm_val}")
    print("-" * 85)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 compiler.py <input.asm> [output.bin]")
        sys.exit(1)

    try:
        with open(sys.argv[1], 'r') as f:
            bytecode = compile_asm(f.read())
        
        out_file = sys.argv[2] if len(sys.argv) > 2 else "program.bin"
        with open(out_file, 'wb') as f:
            f.write(bytecode)
        
        instr_count = len(bytecode) // 12
        print(f"Success! Compiled {instr_count} instructions to '{out_file}'.")
        print_beautiful_bytecode(bytecode)
    except Exception as e:
        print(f"\n[!] Compilation error:\n{e}")
        sys.exit(1)