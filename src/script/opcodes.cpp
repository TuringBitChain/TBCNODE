// Copyright (c) 2020 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file
// LICENSE.
#include "opcodes.h"

#include <iostream>

std::ostream& operator<<(std::ostream& os, const opcodetype& opcode)
{
    if(opcode >= 1 && opcode <= 75)
    {
        os << static_cast<int>(opcode);
        return os;
    }

    switch(opcode)
    {
    case OP_0:
        os << "OP_0";
        break;
    case OP_1:
        os << "OP_1";
        break;
    case OP_2:
        os << "OP_2";
        break;
    case OP_3:
        os << "OP_3";
        break;
    case OP_4:
        os << "OP_4";
        break;
    case OP_5:
        os << "OP_5";
        break;
    case OP_6:
        os << "OP_6";
        break;
    case OP_7:
        os << "OP_7";
        break;
    case OP_8:
        os << "OP_8";
        break;
    case OP_9:
        os << "OP_9";
        break;
    case OP_10:
        os << "OP_10";
        break;
    case OP_11:
        os << "OP_11";
        break;
    case OP_12:
        os << "OP_12";
        break;
    case OP_13:
        os << "OP_13";
        break;
    case OP_14:
        os << "OP_14";
        break;
    case OP_15:
        os << "OP_15";
        break;
    case OP_16:
        os << "OP_16";
        break;
    default:
        os << GetOpName(opcode);
    }
    return os;
}

const char* GetOpName(opcodetype opcode)
{
    switch(opcode)
    {
    // push value
    case OP_0:
        return "0";
    case OP_PUSHDATA1:
        return "OP_PUSHDATA1";
    case OP_PUSHDATA2:
        return "OP_PUSHDATA2";
    case OP_PUSHDATA4:
        return "OP_PUSHDATA4";
    case OP_1NEGATE:
        return "-1";
    case OP_RESERVED:
        return "OP_RESERVED";
    case OP_1:
        return "1";
    case OP_2:
        return "2";
    case OP_3:
        return "3";
    case OP_4:
        return "4";
    case OP_5:
        return "5";
    case OP_6:
        return "6";
    case OP_7:
        return "7";
    case OP_8:
        return "8";
    case OP_9:
        return "9";
    case OP_10:
        return "10";
    case OP_11:
        return "11";
    case OP_12:
        return "12";
    case OP_13:
        return "13";
    case OP_14:
        return "14";
    case OP_15:
        return "15";
    case OP_16:
        return "16";

    // control
    case OP_NOP:
        return "OP_NOP";
    case OP_VER:
        return "OP_VER";
    case OP_IF:
        return "OP_IF";
    case OP_NOTIF:
        return "OP_NOTIF";
    case OP_VERIF:
        return "OP_VERIF";
    case OP_VERNOTIF:
        return "OP_VERNOTIF";
    case OP_ELSE:
        return "OP_ELSE";
    case OP_ENDIF:
        return "OP_ENDIF";
    case OP_VERIFY:
        return "OP_VERIFY";
    case OP_RETURN:
        return "OP_RETURN";

    // stack ops
    case OP_TOALTSTACK:
        return "OP_TOALTSTACK";
    case OP_FROMALTSTACK:
        return "OP_FROMALTSTACK";
    case OP_2DROP:
        return "OP_2DROP";
    case OP_2DUP:
        return "OP_2DUP";
    case OP_3DUP:
        return "OP_3DUP";
    case OP_2OVER:
        return "OP_2OVER";
    case OP_2ROT:
        return "OP_2ROT";
    case OP_2SWAP:
        return "OP_2SWAP";
    case OP_IFDUP:
        return "OP_IFDUP";
    case OP_DEPTH:
        return "OP_DEPTH";
    case OP_DROP:
        return "OP_DROP";
    case OP_DUP:
        return "OP_DUP";
    case OP_NIP:
        return "OP_NIP";
    case OP_OVER:
        return "OP_OVER";
    case OP_PICK:
        return "OP_PICK";
    case OP_ROLL:
        return "OP_ROLL";
    case OP_ROT:
        return "OP_ROT";
    case OP_SWAP:
        return "OP_SWAP";
    case OP_TUCK:
        return "OP_TUCK";

    // splice ops
    case OP_CAT:
        return "OP_CAT";
    case OP_SPLIT:
        return "OP_SPLIT";
    case OP_NUM2BIN:
        return "OP_NUM2BIN";
    case OP_BIN2NUM:
        return "OP_BIN2NUM";
    case OP_SIZE:
        return "OP_SIZE";

    // bit logic
    case OP_INVERT:
        return "OP_INVERT";
    case OP_AND:
        return "OP_AND";
    case OP_OR:
        return "OP_OR";
    case OP_XOR:
        return "OP_XOR";
    case OP_EQUAL:
        return "OP_EQUAL";
    case OP_EQUALVERIFY:
        return "OP_EQUALVERIFY";
    case OP_RESERVED1:
        return "OP_RESERVED1";
    case OP_RESERVED2:
        return "OP_RESERVED2";

    // numeric
    case OP_1ADD:
        return "OP_1ADD";
    case OP_1SUB:
        return "OP_1SUB";
    case OP_2MUL:
        return "OP_2MUL";
    case OP_2DIV:
        return "OP_2DIV";
    case OP_NEGATE:
        return "OP_NEGATE";
    case OP_ABS:
        return "OP_ABS";
    case OP_NOT:
        return "OP_NOT";
    case OP_0NOTEQUAL:
        return "OP_0NOTEQUAL";
    case OP_ADD:
        return "OP_ADD";
    case OP_SUB:
        return "OP_SUB";
    case OP_MUL:
        return "OP_MUL";
    case OP_DIV:
        return "OP_DIV";
    case OP_MOD:
        return "OP_MOD";
    case OP_LSHIFT:
        return "OP_LSHIFT";
    case OP_RSHIFT:
        return "OP_RSHIFT";
    case OP_BOOLAND:
        return "OP_BOOLAND";
    case OP_BOOLOR:
        return "OP_BOOLOR";
    case OP_NUMEQUAL:
        return "OP_NUMEQUAL";
    case OP_NUMEQUALVERIFY:
        return "OP_NUMEQUALVERIFY";
    case OP_NUMNOTEQUAL:
        return "OP_NUMNOTEQUAL";
    case OP_LESSTHAN:
        return "OP_LESSTHAN";
    case OP_GREATERTHAN:
        return "OP_GREATERTHAN";
    case OP_LESSTHANOREQUAL:
        return "OP_LESSTHANOREQUAL";
    case OP_GREATERTHANOREQUAL:
        return "OP_GREATERTHANOREQUAL";
    case OP_MIN:
        return "OP_MIN";
    case OP_MAX:
        return "OP_MAX";
    case OP_WITHIN:
        return "OP_WITHIN";

    // crypto
    case OP_RIPEMD160:
        return "OP_RIPEMD160";
    case OP_SHA1:
        return "OP_SHA1";
    case OP_SHA256:
        return "OP_SHA256";
    case OP_HASH160:
        return "OP_HASH160";
    case OP_HASH256:
        return "OP_HASH256";
    case OP_CODESEPARATOR:
        return "OP_CODESEPARATOR";
    case OP_CHECKSIG:
        return "OP_CHECKSIG";
    case OP_CHECKSIGVERIFY:
        return "OP_CHECKSIGVERIFY";
    case OP_CHECKMULTISIG:
        return "OP_CHECKMULTISIG";
    case OP_CHECKMULTISIGVERIFY:
        return "OP_CHECKMULTISIGVERIFY";

    // expansion
    case OP_NOP1:
        return "OP_NOP1";
    case OP_CHECKLOCKTIMEVERIFY:
        return "OP_CHECKLOCKTIMEVERIFY";
    case OP_CHECKSEQUENCEVERIFY:
        return "OP_CHECKSEQUENCEVERIFY";
    case OP_NOP4:
        return "OP_NOP4";
    case OP_NOP5:
        return "OP_NOP5";
    case OP_NOP6:
        return "OP_NOP6";
    case OP_NOP7:
        return "OP_NOP7";
    case OP_NOP8:
        return "OP_NOP8";
    case OP_NOP9:
        return "OP_NOP9";
    case OP_NOP10:
        return "OP_NOP10";
    case OP_PUSH_META:
        return "OP_PUSH_META";
    case OP_PARTIAL_HASH:
        return "OP_PARTIAL_HASH";
    case OP_CHECKDATASIG:
        return "OP_CHECKDATASIG";
    case OP_CHECKDATASIGVERIFY:
        return "OP_CHECKDATASIGVERIFY";

    case OP_INVALIDOPCODE:
        return "OP_INVALIDOPCODE";

        // Note:
        //  The template matching params OP_SMALLINTEGER/etc are defined in
        //  opcodetype enum as kind of implementation hack, they are *NOT*
        //  real opcodes. If found in real Script, just let the default:
        //  case deal with them.

    default:
        return "OP_UNKNOWN";
    }
}

