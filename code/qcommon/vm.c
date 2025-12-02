/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2012-2020 Quake3e project

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// vm.c -- virtual machine

#include "vm_local.h"

opcode_info_t ops[ OP_MAX ] =
{
	// size, stack, nargs, flags
	{ 0, 0, 0, 0 }, // undef
	{ 0, 0, 0, 0 }, // ignore
	{ 0, 0, 0, 0 }, // break

	{ 4, 0, 0, 0 }, // enter
	{ 4,-4, 0, 0 }, // leave
	{ 0, 0, 1, 0 }, // call
	{ 0, 4, 0, 0 }, // push
	{ 0,-4, 1, 0 }, // pop

	{ 4, 4, 0, 0 }, // const
	{ 4, 4, 0, 0 }, // local
	{ 0,-4, 1, 0 }, // jump

	{ 4,-8, 2, JUMP }, // eq
	{ 4,-8, 2, JUMP }, // ne

	{ 4,-8, 2, JUMP }, // lti
	{ 4,-8, 2, JUMP }, // lei
	{ 4,-8, 2, JUMP }, // gti
	{ 4,-8, 2, JUMP }, // gei

	{ 4,-8, 2, JUMP }, // ltu
	{ 4,-8, 2, JUMP }, // leu
	{ 4,-8, 2, JUMP }, // gtu
	{ 4,-8, 2, JUMP }, // geu

	{ 4,-8, 2, JUMP|FPU }, // eqf
	{ 4,-8, 2, JUMP|FPU }, // nef

	{ 4,-8, 2, JUMP|FPU }, // ltf
	{ 4,-8, 2, JUMP|FPU }, // lef
	{ 4,-8, 2, JUMP|FPU }, // gtf
	{ 4,-8, 2, JUMP|FPU }, // gef

	{ 0, 0, 1, 0 }, // load1
	{ 0, 0, 1, 0 }, // load2
	{ 0, 0, 1, 0 }, // load4
	{ 0,-8, 2, 0 }, // store1
	{ 0,-8, 2, 0 }, // store2
	{ 0,-8, 2, 0 }, // store4
	{ 1,-4, 1, 0 }, // arg
	{ 4,-8, 2, 0 }, // bcopy

	{ 0, 0, 1, 0 }, // sex8
	{ 0, 0, 1, 0 }, // sex16

	{ 0, 0, 1, 0 }, // negi
	{ 0,-4, 3, 0 }, // add
	{ 0,-4, 3, 0 }, // sub
	{ 0,-4, 3, 0 }, // divi
	{ 0,-4, 3, 0 }, // divu
	{ 0,-4, 3, 0 }, // modi
	{ 0,-4, 3, 0 }, // modu
	{ 0,-4, 3, 0 }, // muli
	{ 0,-4, 3, 0 }, // mulu

	{ 0,-4, 3, 0 }, // band
	{ 0,-4, 3, 0 }, // bor
	{ 0,-4, 3, 0 }, // bxor
	{ 0, 0, 1, 0 }, // bcom

	{ 0,-4, 3, 0 }, // lsh
	{ 0,-4, 3, 0 }, // rshi
	{ 0,-4, 3, 0 }, // rshu

	{ 0, 0, 1, FPU }, // negf
	{ 0,-4, 3, FPU }, // addf
	{ 0,-4, 3, FPU }, // subf
	{ 0,-4, 3, FPU }, // divf
	{ 0,-4, 3, FPU }, // mulf

	{ 0, 0, 1, 0 },   // cvif
	{ 0, 0, 1, FPU }  // cvfi
};

const char *opname[ 256 ] = {
	"OP_UNDEF",

	"OP_IGNORE",

	"OP_BREAK",

	"OP_ENTER",
	"OP_LEAVE",
	"OP_CALL",
	"OP_PUSH",
	"OP_POP",

	"OP_CONST",

	"OP_LOCAL",

	"OP_JUMP",

	//-------------------

	"OP_EQ",
	"OP_NE",

	"OP_LTI",
	"OP_LEI",
	"OP_GTI",
	"OP_GEI",

	"OP_LTU",
	"OP_LEU",
	"OP_GTU",
	"OP_GEU",

	"OP_EQF",
	"OP_NEF",

	"OP_LTF",
	"OP_LEF",
	"OP_GTF",
	"OP_GEF",

	//-------------------

	"OP_LOAD1",
	"OP_LOAD2",
	"OP_LOAD4",
	"OP_STORE1",
	"OP_STORE2",
	"OP_STORE4",
	"OP_ARG",

	"OP_BLOCK_COPY",

	//-------------------

	"OP_SEX8",
	"OP_SEX16",

	"OP_NEGI",
	"OP_ADD",
	"OP_SUB",
	"OP_DIVI",
	"OP_DIVU",
	"OP_MODI",
	"OP_MODU",
	"OP_MULI",
	"OP_MULU",

	"OP_BAND",
	"OP_BOR",
	"OP_BXOR",
	"OP_BCOM",

	"OP_LSH",
	"OP_RSHI",
	"OP_RSHU",

	"OP_NEGF",
	"OP_ADDF",
	"OP_SUBF",
	"OP_DIVF",
	"OP_MULF",

	"OP_CVIF",
	"OP_CVFI"
};

// used by Com_Error to get rid of running vm's before longjmp
static int forced_unload;

static struct vm_s vmTable[ VM_COUNT ];

static const char *vmName[ VM_COUNT ] = {
	"qagame",
	"cgame",
	"ui"
};

/*
==============
VM_Init
==============
*/
void VM_Init( void ) {
	Com_Memset( vmTable, 0, sizeof( vmTable ) );
}


/*
===============
VM_ValueToSymbol

Assumes a program counter value
===============
*/
const char *VM_ValueToSymbol( vm_t *vm, int value ) {
	vmSymbol_t	*sym;
	static char		text[MAX_TOKEN_CHARS];

	sym = vm->symbols;
	if ( !sym ) {
		return "NO SYMBOLS";
	}

	// find the symbol
	while ( sym->next && sym->next->symValue <= value ) {
		sym = sym->next;
	}

	if ( value == sym->symValue ) {
		return sym->symName;
	}

	Com_sprintf( text, sizeof( text ), "%s+%i", sym->symName, value - sym->symValue );

	return text;
}


/*
===============
VM_ValueToFunctionSymbol

For profiling, find the symbol behind this value
===============
*/
vmSymbol_t *VM_ValueToFunctionSymbol( vm_t *vm, int value ) {
	vmSymbol_t	*sym;
	static vmSymbol_t	nullSym;

	sym = vm->symbols;
	if ( !sym ) {
		return &nullSym;
	}

	while ( sym->next && sym->next->symValue <= value ) {
		sym = sym->next;
	}

	return sym;
}


/*
===============
VM_SymbolToValue
===============
*/
int VM_SymbolToValue( vm_t *vm, const char *symbol ) {
	vmSymbol_t	*sym;

	for ( sym = vm->symbols ; sym ; sym = sym->next ) {
		if ( !strcmp( symbol, sym->symName ) ) {
			return sym->symValue;
		}
	}
	return 0;
}

static void VM_SwapLongs( void *data, int length )
{
#ifndef Q3_LITTLE_ENDIAN
	int32_t *ptr;
	int i;
	ptr = (int32_t *) data;
	length /= sizeof( int32_t );
	for ( i = 0; i < length; i++ ) {
		ptr[ i ] = LittleLong( ptr[ i ] );
	}
#endif
}

/*
=================
VM_ValidateHeader
=================
*/
static char *VM_ValidateHeader( vmHeader_t *header, int fileSize )
{
	static char errMsg[128];
	int n;

	// truncated
	if ( fileSize < ( sizeof( vmHeader_t ) - sizeof( int32_t ) ) ) {
		sprintf( errMsg, "truncated image header (%i bytes long)", fileSize );
		return errMsg;
	}

	// bad magic
	if ( LittleLong( header->vmMagic ) != VM_MAGIC_VER3 ) {
		sprintf( errMsg, "bad file magic %08x", LittleLong( header->vmMagic ) );
		return errMsg;
	}

	// truncated
	if ( fileSize < sizeof( vmHeader_t ) && LittleLong( header->vmMagic ) != VM_MAGIC_VER3 ) {
		sprintf( errMsg, "truncated image header (%i bytes long)", fileSize );
		return errMsg;
	}

	if ( LittleLong( header->vmMagic ) == VM_MAGIC_VER3 )
		n = sizeof( vmHeader_t );
	else
		n = ( sizeof( vmHeader_t ) - sizeof( int32_t ) );

	// byte swap the header
	VM_SwapLongs( header, n );

	// bad code offset
	if ( header->codeOffset >= fileSize ) {
		sprintf( errMsg, "bad code segment offset %i", header->codeOffset );
		return errMsg;
	}

	// bad code length
	if ( header->codeLength <= 0 || header->codeOffset + header->codeLength > fileSize ) {
		sprintf( errMsg, "bad code segment length %i", header->codeLength );
		return errMsg;
	}

	// bad data offset
	if ( header->dataOffset >= fileSize || header->dataOffset != header->codeOffset + header->codeLength ) {
		sprintf( errMsg, "bad data segment offset %i", header->dataOffset );
		return errMsg;
	}

	// bad data length
	if ( header->dataOffset + header->dataLength > fileSize )  {
		sprintf( errMsg, "bad data segment length %i", header->dataLength );
		return errMsg;
	}

	if ( header->vmMagic == VM_MAGIC_VER3 ) {
		// bad lit/jtrg length
		if ( header->dataOffset + header->dataLength + header->litLength + header->jtrgLength != fileSize ) {
			sprintf( errMsg, "bad lit/jtrg segment length" );
			return errMsg;
		}
	}
	// bad lit length
	else if ( header->dataOffset + header->dataLength + header->litLength != fileSize ) {
		sprintf( errMsg, "bad lit segment length %i", header->litLength );
		return errMsg;
	}

	return NULL;
}


/*
=================
VM_LoadQVM

Load a .qvm file

if ( alloc )
 - Validate header, swap data
 - Alloc memory for data/instructions
 - Alloc memory for instructionPointers - NOT NEEDED
 - Load instructions
 - Clear/load data
else
 - Check for header changes
 - Clear/load data

=================
*/
static vmHeader_t *VM_LoadQVM( vm_t *vm, qboolean alloc ) {
	int					length;
	unsigned int		dataLength;
	unsigned int		dataAlloc;
	char				filename[MAX_QPATH], *errorMsg;
	unsigned int		crc32sum;
	vmHeader_t			*header;

	// load the image
	Com_sprintf( filename, sizeof(filename), "qvm/%s/%s.qvm", cl_changeqvm->string, vm->name );
	Com_Printf( "Loading vm file %s...\n", filename );
	length = FS_ReadFile( filename, (void **)&header );

	if ( !header ) {
		Com_sprintf( filename, sizeof(filename), "qvm/%s.qvm", vm->name );
		Com_Printf( "Loading vm file %s...\n", filename );
		length = FS_ReadFile( filename, (void **)&header );
		if ( !header ) {
			Com_Printf( "Failed.\n" );
			VM_Free( vm );
			return NULL;
		}
	}

	crc32sum = crc32_buffer( (const byte*) header, length );

	// will also swap header
	errorMsg = VM_ValidateHeader( header, length );
	if ( errorMsg ) {
		VM_Free( vm );
		FS_FreeFile( header );
		Com_Printf( S_COLOR_RED "%s\n", errorMsg );
		return NULL;
	}

	vm->crc32sum = crc32sum;

	Com_Printf( "...which has vmMagic VM_MAGIC_VER3\n" );

	vm->exactDataLength = header->dataLength + header->litLength + header->bssLength;

	dataLength = vm->exactDataLength;
	if ( dataLength < PROGRAM_STACK_SIZE ) {
		dataLength = PROGRAM_STACK_SIZE;
	}

	vm->programStackExtra = PROGRAM_STACK_EXTRA;

	// if rounding difference is larger than extra space we need then reuse it
	if ( log2pad( dataLength, 1 ) - dataLength >= PROGRAM_STACK_EXTRA ) {
		// reuse it all for release builds
		vm->programStackExtra = log2pad( dataLength, 1 ) - dataLength;
	} else {
		dataLength += vm->programStackExtra;
	}

	vm->dataLength = dataLength;

	// round up to next power of 2 so all data operations can be mask protected
	dataLength = log2pad( dataLength, 1 );

	// reserve some space for effective LOCAL+LOAD* checks
	dataAlloc = dataLength + VM_DATA_GUARD_SIZE;

	if ( dataLength >= (1U<<31) || dataAlloc >= (1U<<31) ) {
		// dataLenth is negative int32
		VM_Free( vm );
		FS_FreeFile( header );
		Com_Printf( S_COLOR_RED "%s: data segment is too large\n", __func__ );
		return NULL;
	}

	if ( alloc ) {
		// allocate zero filled space for initialized and uninitialized data
		vm->dataBase = Hunk_Alloc( dataAlloc, h_high );
		vm->dataMask = dataLength - 1;
		vm->dataAlloc = dataAlloc;
	} else {
		// clear the data, but make sure we're not clearing more than allocated
		if ( vm->dataAlloc != dataAlloc ) {
			VM_Free( vm );
			FS_FreeFile( header );
			Com_Printf( S_COLOR_YELLOW "Warning: Data region size of %s not matching after"
					"VM_Restart()\n", filename );
			return NULL;
		}
		Com_Memset( vm->dataBase, 0x0, vm->dataAlloc );
	}

	// copy the intialized data
	Com_Memcpy( vm->dataBase, (byte *)header + header->dataOffset, header->dataLength + header->litLength );

	// byte swap the longs
	VM_SwapLongs( vm->dataBase, header->dataLength );

	if( header->vmMagic == VM_MAGIC_VER3 ) {
		int previousNumJumpTableTargets = vm->numJumpTableTargets;

		header->jtrgLength &= ~0x03;

		vm->numJumpTableTargets = header->jtrgLength >> 2;
		Com_Printf( "Loading %d jump table targets\n", vm->numJumpTableTargets );

		if ( alloc ) {
			vm->jumpTableTargets = (int32_t *) Hunk_Alloc( header->jtrgLength, h_high );
		} else {
			if ( vm->numJumpTableTargets != previousNumJumpTableTargets ) {
				VM_Free( vm );
				FS_FreeFile( header );

				Com_Printf( S_COLOR_YELLOW "Warning: Jump table size of %s not matching after "
					"VM_Restart()\n", filename );
				return NULL;
			}

			Com_Memset( vm->jumpTableTargets, 0, header->jtrgLength );
		}

		Com_Memcpy( vm->jumpTableTargets, (byte *)header + header->dataOffset +
				header->dataLength + header->litLength, header->jtrgLength );

		// byte swap the longs
		VM_SwapLongs( vm->jumpTableTargets, header->jtrgLength );
	} else {
		Com_Printf( "QVM file is not VM_MAGIC_VER3\n" );
	}

	return header;
}


static void VM_IgnoreInstructions( instruction_t *buf, const int count ) {
	int i;

	for ( i = 0; i < count; i++ ) {
		Com_Memset( buf + i, 0, sizeof( *buf ) );
		buf[i].op = OP_IGNORE;
	}

	buf[0].value = count > 0 ? count - 1 : 0;
}


static int InvertCondition( int op )
{
	switch ( op ) {
		case OP_EQ: return OP_NE;   // == -> !=
		case OP_NE: return OP_EQ;   // != -> ==

		case OP_LTI: return OP_GEI;	// <  -> >=
		case OP_LEI: return OP_GTI;	// <= -> >
		case OP_GTI: return OP_LEI; // >  -> <=
		case OP_GEI: return OP_LTI; // >= -> <

		case OP_LTU: return OP_GEU;
		case OP_LEU: return OP_GTU;
		case OP_GTU: return OP_LEU;
		case OP_GEU: return OP_LTU;

		case OP_EQF: return OP_NEF;
		case OP_NEF: return OP_EQF;

		case OP_LTF: return OP_GEF;
		case OP_LEF: return OP_GTF;
		case OP_GTF: return OP_LEF;
		case OP_GEF: return OP_LTF;

		default: 
			Com_Error( ERR_DROP, "incorrect condition opcode %i", op );
			return op;
	}
}


/*
=================
VM_FindLocal

search for specified local variable until end of function
=================
*/
static qboolean VM_FindLocal( int32_t addr, const instruction_t *buf, const instruction_t *end, int32_t *back_addr ) {
	int32_t curr_addr = *back_addr;
	while ( buf < end ) {
		if ( buf->op == OP_LOCAL ) {
			if ( buf->value == addr ) {
				return qtrue;
			}
			++buf; continue;
		}
		if ( ops[ buf->op ].flags & JUMP ) {
			if ( buf->value < curr_addr ) {
				curr_addr = buf->value;
			}
			++buf; continue;
		}
		if ( buf->op == OP_JUMP ) {
			if ( buf->value && buf->value < curr_addr ) {
				curr_addr = buf->value;
			}
			++buf; continue;
		}
		if ( buf->op == OP_PUSH && (buf+1)->op == OP_LEAVE ) {
			break;
		}
		++buf;
	}
	*back_addr = curr_addr;
	return qfalse;
}


/*
=================
VM_Fixup

Do some corrections to fix known Q3LCC flaws
=================
*/
static void VM_Fixup( instruction_t *buf, int instructionCount )
{
	int n;
	instruction_t *i;

	i = buf;
	n = 0;

	while ( n < instructionCount )
	{
		if ( i->op == OP_LOCAL ) {

			// skip useless sequences
			if ( (i+1)->op == OP_LOCAL && (i+0)->value == (i+1)->value && (i+2)->op == OP_LOAD4 && (i+3)->op == OP_STORE4 ) {
				VM_IgnoreInstructions( i, 4 );
				i += 4; n += 4;
				continue;
			}

			// [0]OP_LOCAL + [1]OP_CONST + [2]OP_CALL + [3]OP_STORE4
			if ( (i+1)->op == OP_CONST && (i+2)->op == OP_CALL && (i+3)->op == OP_STORE4 && !(i+4)->jused ) {
				// [4]OP_CONST|OP_LOCAL (dest) + [5]OP_LOCAL(temp) + [6]OP_LOAD4 + [7]OP_STORE4
				if ( (i+4)->op == OP_CONST || (i+4)->op == OP_LOCAL ) {
					if ( (i+5)->op == OP_LOCAL && (i+5)->value == (i+0)->value && (i+6)->op == OP_LOAD4 && (i+7)->op == OP_STORE4 ) {
						int32_t back_addr = n;
						int32_t curr_addr = n;
						qboolean do_break = qfalse;

						// make sure that address of (potentially) temporary variable is not referenced further in this function
						if ( VM_FindLocal( i->value, i + 8, buf + instructionCount, &back_addr ) ) {
							i++; n++;
							continue;
						}

						// we have backward jumps in code then check for references before current position
						while ( back_addr < curr_addr ) {
							curr_addr = back_addr;
							if ( VM_FindLocal( i->value, buf + back_addr, i, &back_addr ) ) {
								do_break = qtrue;
								break;
							}
						}
						if ( do_break ) {
							i++; n++;
							continue;
						}

						(i+0)->op = (i+4)->op;
						(i+0)->value = (i+4)->value;
						VM_IgnoreInstructions( i + 4, 4 );
						i += 8;
						n += 8;
						continue;
					}
				}
			}
		}

		if ( i->op == OP_LEAVE && !i->endp ) {
			if ( !(i+1)->jused && (i+1)->op == OP_CONST && (i+2)->op == OP_JUMP ) {
				int v = (i+1)->value;
				if ( buf[ v ].op == OP_PUSH && buf[ v+1 ].op == OP_LEAVE && buf[ v+1 ].endp ) {
					VM_IgnoreInstructions( i + 1, 2 );
					i += 3;
					n += 3;
					continue;
				}
			}
		}

		//n + 0: if ( cond ) goto label1;
		//n + 2: goto label2;
		//n + 3: label1:
		// ...
		//n + x: label2:
		if ( ( ops[i->op].flags & (JUMP | FPU) ) == JUMP && !(i+1)->jused && (i+1)->op == OP_CONST && (i+2)->op == OP_JUMP ) {
			if ( i->value == n + 3 && (i+1)->value >= n + 3 ) {
				i->op = InvertCondition( i->op );
				i->value = ( i + 1 )->value;
				VM_IgnoreInstructions( i + 1, 2 );
				i += 3;
				n += 3;
				continue;
			}
		}
		i++;
		n++;
	}
}


/*
=================
VM_LoadInstructions

loads instructions in structured format
=================
*/
const char *VM_LoadInstructions( const byte *code_pos, int codeLength, int instructionCount, instruction_t *buf )
{
	static char errBuf[ 128 ];
	const byte *code_start, *code_end;
	int i, n, op0, op1, opStack;
	instruction_t *ci;

	code_start = code_pos; // for printing
	code_end = code_pos + codeLength;

	ci = buf;
	opStack = 0;
	op1 = OP_UNDEF;

	// load instructions and perform some initial calculations/checks
	for ( i = 0; i < instructionCount; i++, ci++, op1 = op0 ) {
		op0 = *code_pos;
		if ( op0 < 0 || op0 >= OP_MAX ) {
			sprintf( errBuf, "bad opcode %02X at offset %d", op0, (int)(code_pos - code_start) );
			return errBuf;
		}
		n = ops[ op0 ].size;
		if ( code_pos + 1 + n  > code_end ) {
			sprintf( errBuf, "code_pos > code_end" );
			return errBuf;
		}
		code_pos++;
		ci->op = op0;
		if ( n == 4 ) {
			ci->value = LittleLong( *((int32_t*)code_pos) );
			code_pos += 4;
		} else if ( n == 1 ) {
			ci->value = *((unsigned char*)code_pos);
			code_pos += 1;
		} else {
			ci->value = 0;
		}

		if ( ops[ op0 ].flags & FPU ) {
			ci->fpu = 1;
		}

		// setup jump value from previous const
		if ( op0 == OP_JUMP && op1 == OP_CONST ) {
			ci->value = (ci-1)->value;
		}

		ci->opStack = opStack;
		opStack += ops[ op0 ].stack;
	}

	return NULL;
}


static qboolean safe_address( instruction_t *ci, instruction_t *proc, int dataLength )
{
	if ( ci->op == OP_LOCAL ) {
		// local address can't exceed programStack frame plus 256 bytes of passed arguments
		if ( ci->value < 8 || ( proc && ci->value >= proc->value + 256 ) )
			return qfalse;
		return qtrue;
	}

	if ( ci->op == OP_CONST ) {
		// constant address can't exceed data segment
		if ( ci->value >= dataLength || ci->value < 0 )
			return qfalse;
		return qtrue;
	}

	return qfalse;
}


/*
===============================
VM_CheckInstructions

performs additional consistency and security checks
===============================
*/
const char *VM_CheckInstructions( instruction_t *buf,
								int instructionCount,
								const int32_t *jumpTableTargets,
								int numJumpTableTargets,
								int dataLength )
{
	static char errBuf[ 128 ];
	instruction_t *opStackPtr[ PROC_OPSTACK_SIZE ];
	int i, m, n, v, op0, op1, opStack, pstack;
	instruction_t *ci, *proc;
	int startp, endp;
	int safe_stores;
	int unsafe_stores;

	ci = buf;
	opStack = 0;

	// opstack checks
	for ( i = 0; i < instructionCount; i++, ci++ ) {
		opStack += ops[ ci->op ].stack;
		if ( opStack < 0 ) {
			sprintf( errBuf, "opStack underflow at %i", i );
			return errBuf;
		}
		if ( opStack >= PROC_OPSTACK_SIZE * 4 ) {
			sprintf( errBuf, "opStack overflow at %i", i );
			return errBuf;
		}
	}

	ci = buf;
	pstack = 0;
	opStack = 0;
	safe_stores = 0;
	unsafe_stores = 0;
	op1 = OP_UNDEF;
	proc = NULL;
	Com_Memset( opStackPtr, 0, sizeof( opStackPtr ) );

	startp = 0;
	endp = instructionCount - 1;

	// Additional security checks

	for ( i = 0; i < instructionCount; i++, ci++, op1 = op0 ) {
		op0 = ci->op;

		m = ops[ ci->op ].stack;
		opStack += m;
		if ( m >= 0 ) {
			// do some FPU type promotion for more efficient loads
			if ( ci->fpu && ci->op != OP_CVIF ) {
				opStackPtr[ opStack / 4 ]->fpu = 1;
			}
			opStackPtr[ opStack >> 2 ] = ci;
		} else {
			if ( ci->fpu ) {
				if ( m <= -8 ) {
					opStackPtr[ opStack / 4 + 1 ]->fpu = 1;
					opStackPtr[ opStack / 4 + 2 ]->fpu = 1;
				} else {
					opStackPtr[ opStack / 4 + 0 ]->fpu = 1;
					opStackPtr[ opStack / 4 + 1 ]->fpu = 1;
				}
			} else {
				if ( m <= -8 ) {
					//
				} else {
					opStackPtr[ opStack / 4 + 0 ] = ci;
				}
			}
		}

		// function entry
		if ( op0 == OP_ENTER ) {
			// missing block end
			if ( proc || ( pstack && op1 != OP_LEAVE ) ) {
				sprintf( errBuf, "missing proc end before %i", i );
				return errBuf;
			}
			if ( ci->opStack != 0 ) {
				v = ci->opStack;
				sprintf( errBuf, "bad entry opstack %i at %i", v, i );
				return errBuf;
			}
			v = ci->value;
			if ( v < 0 || v >= PROGRAM_STACK_SIZE || (v & 3) ) {
				sprintf( errBuf, "bad entry programStack %i at %i", v, i );
				return errBuf;
			}

			pstack = ci->value;

			// mark jump target
			ci->jused = 1;
			proc = ci;
			startp = i + 1;

			// locate endproc
			for ( endp = 0, n = i+1 ; n < instructionCount; n++ ) {
				if ( buf[n].op == OP_PUSH && buf[n+1].op == OP_LEAVE ) {
					buf[n+1].endp = 1;
					endp = n;
					break;
				}
			}

			if ( endp == 0 ) {
				sprintf( errBuf, "missing end proc for %i", i );
				return errBuf;
			}

			continue;
		}

		// proc opstack will carry max.possible opstack value
		if ( proc && ci->opStack > proc->opStack )
			proc->opStack = ci->opStack;

		// function return
		if ( op0 == OP_LEAVE ) {
			// bad return programStack
			if ( pstack != ci->value ) {
				v = ci->value;
				sprintf( errBuf, "bad programStack %i at %i", v, i );
				return errBuf;
			}
			// bad opStack before return
			if ( ci->opStack != 4 ) {
				v = ci->opStack;
				sprintf( errBuf, "bad opStack %i at %i", v, i );
				return errBuf;
			}
			v = ci->value;
			if ( v < 0 || v >= PROGRAM_STACK_SIZE || (v & 3) ) {
				sprintf( errBuf, "bad return programStack %i at %i", v, i );
				return errBuf;
			}
			if ( op1 == OP_PUSH ) {
				if ( proc == NULL ) {
					sprintf( errBuf, "unexpected proc end at %i", i );
					return errBuf;
				}
				proc = NULL;
				startp = i + 1; // next instruction
				endp = instructionCount - 1; // end of the image
			}
			continue;
		}

		// conditional jumps
		if ( ops[ ci->op ].flags & JUMP ) {
			v = ci->value;
			// conditional jumps should have opStack >= 8
			if ( ci->opStack < 8 ) {
				sprintf( errBuf, "bad jump opStack %i at %i", ci->opStack, i );
				return errBuf;
			}
			//if ( v >= header->instructionCount ) {
			// allow only local proc jumps
			if ( v < startp || v > endp ) {
				sprintf( errBuf, "jump target %i at %i is out of range (%i,%i)", v, i-1, startp, endp );
				return errBuf;
			}
			if ( buf[v].opStack != ci->opStack - 8 ) {
				n = buf[v].opStack;
				sprintf( errBuf, "jump target %i has bad opStack %i", v, n );
				return errBuf;
			}
			// mark jump target
			buf[v].jused = 1;
			continue;
		}

		// unconditional jumps
		if ( op0 == OP_JUMP ) {
			// jumps should have opStack >= 4
			if ( ci->opStack < 4 ) {
				sprintf( errBuf, "bad jump opStack %i at %i", ci->opStack, i );
				return errBuf;
			}
			if ( op1 == OP_CONST ) {
				v = buf[i-1].value;
				// allow only local jumps
				if ( v < startp || v > endp ) {
					sprintf( errBuf, "jump target %i at %i is out of range (%i,%i)", v, i-1, startp, endp );
					return errBuf;
				}
				if ( buf[v].opStack != ci->opStack - 4 ) {
					n = buf[v].opStack;
					sprintf( errBuf, "jump target %i has bad opStack %i", v, n );
					return errBuf;
				}
				if ( buf[v].op == OP_ENTER ) {
					n = buf[v].op;
					sprintf( errBuf, "jump target %i has bad opcode %s", v, opname[ n ] );
					return errBuf;
				}
				if ( v == (i-1) ) {
					sprintf( errBuf, "self loop at %i", v );
					return errBuf;
				}
				// mark jump target
				buf[v].jused = 1;
			} else {
				if ( proc )
					proc->swtch = 1;
				else
					ci->swtch = 1;
			}
			continue;
		}

		if ( op0 == OP_CALL ) {
			if ( ci->opStack < 4 ) {
				sprintf( errBuf, "bad call opStack at %i", i );
				return errBuf;
			}
			if ( op1 == OP_CONST ) {
				v = buf[i-1].value;
				// analyse only local function calls
				if ( v >= 0 ) {
					if ( v >= instructionCount ) {
						sprintf( errBuf, "call target %i is out of range", v );
						return errBuf;
					}
					if ( buf[v].op != OP_ENTER ) {
						n = buf[v].op;
						sprintf( errBuf, "call target %i has bad opcode %s", v, opname[ n ] );
						return errBuf;
					}
					if ( v == 0 ) {
						sprintf( errBuf, "explicit vmMain call inside VM at %i", i );
						return errBuf;
					}
					// mark jump target
					buf[v].jused = 1;
				}
			}
			continue;
		}

		if ( ci->op == OP_ARG ) {
			v = ci->value & 255;
			if ( proc == NULL ) {
				sprintf( errBuf, "missing proc frame for %s %i at %i", opname[ ci->op ], v, i );
				return errBuf;
			}
			// argument can't exceed programStack frame
			if ( v < 8 || v > pstack - 4 || (v & 3) ) {
				sprintf( errBuf, "bad argument address %i at %i", v, i );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_LOCAL ) {
			v = ci->value;
			if ( proc == NULL ) {
				sprintf( errBuf, "missing proc frame for %s %i at %i", opname[ ci->op ], v, i );
				return errBuf;
			}
			if ( (ci+1)->op == OP_LOAD4 || (ci+1)->op == OP_LOAD2 || (ci+1)->op == OP_LOAD1 ) {
				if ( !safe_address( ci, proc, dataLength ) ) {
					sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i );
					return errBuf;
				}
			}
			continue;
		}

		if ( ci->op == OP_LOAD4 && op1 == OP_CONST ) {
			v = (ci-1)->value;
			if ( v < 0 || v > dataLength - 4 ) {
				sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i - 1 );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_LOAD2 && op1 == OP_CONST ) {
			v = (ci-1)->value;
			if ( v < 0 || v > dataLength - 2 ) {
				sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i - 1 );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_LOAD1 && op1 == OP_CONST ) {
			v =  (ci-1)->value;
			if ( v < 0 || v > dataLength - 1 ) {
				sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i - 1 );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_STORE4 || ci->op == OP_STORE2 || ci->op == OP_STORE1 ) {
			instruction_t *x = opStackPtr[ opStack / 4 + 1 ];
			if ( x->op == OP_CONST || x->op == OP_LOCAL ) {
				if ( safe_address( x, proc, dataLength ) ) {
					ci->safe = 1;
					safe_stores++;
					continue;
				} else {
					sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], x->value, (int)(x - buf) );
					return errBuf;
				}
			}
			unsafe_stores++;
			continue;
		}

		if ( ci->op == OP_BLOCK_COPY ) {
			instruction_t *src = opStackPtr[ opStack / 4 + 2 ];
			instruction_t *dst = opStackPtr[ opStack / 4 + 1 ];
			int safe = 0;
			v = ci->value;
			if ( v >= dataLength ) {
				sprintf( errBuf, "bad count %i for block copy at %i", v, i - 1 );
				return errBuf;
			}
			if ( src->op == OP_LOCAL || src->op == OP_CONST ) {
				if ( !safe_address( src, proc, dataLength ) ) {
					sprintf( errBuf, "bad src for block copy at %i", (int)(dst - buf) );
					return errBuf;
				}
				src->safe = 1;
				safe++;
			}
			if ( dst->op == OP_LOCAL || dst->op == OP_CONST ) {
				if ( !safe_address( dst, proc, dataLength ) ) {
					sprintf( errBuf, "bad dst for block copy at %i", (int)(dst - buf) );
					return errBuf;
				}
				dst->safe = 1;
				safe++;
			}
			if ( safe == 2 ) {
				ci->safe = 1;
			}
		}
	}

	if ( ( safe_stores + unsafe_stores ) > 0 ) {
		Com_DPrintf( "%s: safe stores - %i (%i%%)\n", __func__, safe_stores, safe_stores * 100 / ( safe_stores + unsafe_stores ) );
	}

	if ( op1 != OP_UNDEF && op1 != OP_LEAVE ) {
		sprintf( errBuf, "missing return instruction at the end of the image" );
		return errBuf;
	}

	// ensure that the optimization pass knows about all the jump table targets
	if ( jumpTableTargets ) {
		// first pass - validate
		for( i = 0; i < numJumpTableTargets; i++ ) {
			n = jumpTableTargets[ i ];
			if ( n < 0 || n >= instructionCount ) {
				Com_Printf( S_COLOR_YELLOW "jump target %i set on instruction %i that is out of range [0..%i]",
					i, n, instructionCount - 1 );
				break;
			}
			if ( buf[n].opStack != 0 ) {
				Com_Printf( S_COLOR_YELLOW "jump target %i set on instruction %i (%s) with bad opStack %i\n",
					i, n, opname[ buf[n].op ], buf[n].opStack );
				break;
			}
		}
		if ( i != numJumpTableTargets ) {
			// we may trap this on buggy VM_MAGIC_VER3 images
			// but we can safely optimize code even without JTRGSEG
			goto __noJTS;
		}
		// second pass - apply
		for( i = 0; i < numJumpTableTargets; i++ ) {
			n = jumpTableTargets[ i ];
			buf[ n ].jused = 1;
		}
	} else {
__noJTS:
		v = 0;
		// instructions with opStack > 0 can't be jump labels so it is safe to optimize/merge
		for ( i = 0, ci = buf; i < instructionCount; i++, ci++ ) {
			if ( ci->op == OP_ENTER ) {
				v = ci->swtch;
				continue;
			}
			// if there is a switch statement in function -
			// mark all potential jump labels
			if ( ci->swtch )
				v = ci->swtch;
			if ( ci->opStack > 0 )
				ci->jused = 0;
			else if ( v )
				ci->jused = 1;
		}
	}

	VM_Fixup( buf, instructionCount );

	return NULL;
}

/*
=================
VM_Restart

Reload the data, but leave everything else in place
This allows a server to do a map_restart without changing memory allocation
=================
*/
vm_t *VM_Restart( vm_t *vm ) {
	vmHeader_t	*header;

	// load the image
	if( ( header = VM_LoadQVM( vm, qfalse ) ) == NULL ) {
		Com_Printf( S_COLOR_RED "VM_Restart() failed\n" );
		return NULL;
	}

	Com_Printf( "VM_Restart()\n" );

	// free the original file
	FS_FreeFile( header );

	return vm;
}

/*
================
VM_Create

If image ends in .qvm it will be compiled or interpreted
================
*/
vm_t *VM_Create( vmIndex_t index, syscall_t systemCalls ) {
	int			remaining;
	const char	*name;
	vmHeader_t	*header;
	vm_t		*vm;

	if ( !systemCalls ) {
		Com_Error( ERR_FATAL, "VM_Create: bad parms" );
	}

	if ( (unsigned)index >= VM_COUNT ) {
		Com_Error( ERR_DROP, "VM_Create: bad vm index %i", index );
	}

	remaining = Hunk_MemoryRemaining();

	vm = &vmTable[ index ];

	// see if we already have the VM
	if ( vm->name ) {
		if ( vm->index != index ) {
			Com_Error( ERR_DROP, "VM_Create: bad allocated vm index %i", vm->index );
			return NULL;
		}
		return vm;
	}

	name = vmName[ index ];

	vm->name = name;
	vm->index = index;
	vm->systemCall = systemCalls;

	// load the image
	if( ( header = VM_LoadQVM( vm, qtrue ) ) == NULL ) {
		return NULL;
	}

	// allocate space for the jump targets, which will be filled in by the compile/prep functions
	vm->instructionCount = header->instructionCount;
	vm->instructionPointers = NULL;

	// copy or compile the instructions
	vm->codeLength = header->codeLength;

	// the stack is implicitly at the end of the image
	vm->programStack = vm->dataMask + 1;
	vm->stackBottom = vm->programStack - PROGRAM_STACK_SIZE - vm->programStackExtra;

	vm->compiled = qfalse;

	Com_Printf( "Compiling qvm%i - %s\n", vm->index, vm->name );
	if ( VM_Compile( vm, header ) ) {
		vm->compiled = qtrue;
		Com_Printf( "Compiling qvm%i - %s done\n", vm->index, vm->name );
	}

	// VM_Compile may have reset vm->compiled if compilation failed
	if ( !vm->compiled ) {
		Com_Printf( "Failed to compile qvm%i - %s. Using interpreter\n", vm->index, vm->name );
		if ( !VM_PrepareInterpreter( vm, header ) ) {
			Com_Printf( "Failed to interpret qvm\n" );
			FS_FreeFile( header );	// free the original file
			VM_Free( vm );
			return NULL;
		}
	}

	// free the original file
	FS_FreeFile( header );

	Com_Printf( "%s loaded in %d bytes on the hunk\n", vm->name, remaining - Hunk_MemoryRemaining() );

	return vm;
}

/*
==============
VM_Free
==============
*/
void VM_Free( vm_t *vm ) {

	if( !vm ) {
		return;
	}

	if ( vm->callLevel ) {
		if ( !forced_unload ) {
			Com_Error( ERR_FATAL, "VM_Free(%s) on running vm", vm->name );
			return;
		} else {
			Com_Printf( "forcefully unloading %s vm\n", vm->name );
		}
	}

	if ( vm->destroy )
		vm->destroy( vm );

	Com_Memset( vm, 0, sizeof( *vm ) );
}


void VM_Clear( void ) {
	int i;
	for ( i = 0; i < VM_COUNT; i++ ) {
		VM_Free( &vmTable[ i ] );
	}
}


void VM_Forced_Unload_Start(void) {
	forced_unload = 1;
}


void VM_Forced_Unload_Done(void) {
	forced_unload = 0;
}


/*
==============
VM_Call


Upon a system call, the stack will look like:

sp+32	parm1
sp+28	parm0
sp+24	return value
sp+20	return address
sp+16	local1
sp+14	local0
sp+12	arg1
sp+8	arg0
sp+4	return stack
sp		return address

An interpreted function will immediately execute
an OP_ENTER instruction, which will subtract space for
locals from sp
==============
*/

intptr_t QDECL VM_Call( vm_t *vm, int nargs, int callnum, ... ) {
	intptr_t r;
	int i;

	if ( !vm ) {
		Com_Error( ERR_FATAL, "VM_Call with NULL vm" );
	}

	++vm->callLevel;
#if id386 && !defined __clang__ // calling convention doesn't need conversion in some cases
	if ( vm->compiled )
		r = VM_CallCompiled( vm, nargs+1, (int32_t*)&callnum );
	else
		r = VM_CallInterpreted( vm, nargs+1, (int32_t*)&callnum );
#else
	int32_t args[MAX_VMMAIN_CALL_ARGS];
	va_list ap;

	args[0] = callnum;
	va_start( ap, callnum );
	for ( i = 0; i < nargs; i++ ) {
		args[i+1] = va_arg( ap, int32_t );
	}
	va_end(ap);

	if ( vm->compiled )
		r = VM_CallCompiled( vm, nargs+1, &args[0] );
	else
		r = VM_CallInterpreted( vm, nargs+1, &args[0] );
#endif
	--vm->callLevel;

	return r;
}
