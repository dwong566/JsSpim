/* SPIM S20 MIPS simulator.
   Terminal interface for SPIM simulator.

   Copyright (c) 1990-2015, James R. Larus.
   All rights reserved.

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.

   Neither the name of the James R. Larus nor the names of its contributors may be
   used to endorse or promote products derived from this software without specific
   prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
   GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <unistd.h>
#include <stdio.h>

#include <termios.h>
#include <stdarg.h>

#include "spim.h"
#include "string-stream.h"
#include "spim-utils.h"
#include "inst.h"
#include "reg.h"
#include "mem.h"
#include "data.h"

using namespace emscripten;

bool bare_machine;        /* => simulate bare machine */
bool delayed_branches;        /* => simulate delayed branches */
bool delayed_loads;        /* => simulate delayed loads */
bool accept_pseudo_insts = true;    /* => parse pseudo instructions  */
bool quiet;            /* => no warning messages */
char *exception_file_name;
port message_out, console_out, console_in;
bool mapped_io;            /* => activate memory-mapped IO */
int spim_return_value;        /* Value returned when spim exits */


bool step() {
  mem_addr addr = PC == 0 ? starting_address() : PC;

  bool continuable;
  if (run_program(addr, 1, false, true, &continuable))
    printf("Breakpoint encountered at 0x%08x\n", PC);

  if (!continuable)
    printf("\n");

  return continuable;
}

EMSCRIPTEN_BINDINGS(step) { function("step", &step); }

void run() {
  bool continuable;
  if (run_program(starting_address(), DEFAULT_RUN_STEPS, false, false, &continuable))
    printf("Breakpoint encountered at 0x%08x\n", PC);
  printf("\n");
}

EMSCRIPTEN_BINDINGS(run) { function("run", &run); }

void conti() {
  if (PC == 0) return;

  bool continuable;
  if (run_program(PC, DEFAULT_RUN_STEPS, false, true, &continuable))
    printf("Breakpoint encountered at 0x%08x\n", PC);
  printf("\n");
}

EMSCRIPTEN_BINDINGS(conti) { function("conti", &conti); }

void init(std::string filename) {
  initialize_world(DEFAULT_EXCEPTION_HANDLER, false);
  initialize_run_stack(0, NULL);
  read_assembly_file((char *) filename.c_str());
}
EMSCRIPTEN_BINDINGS(init) { function("init", &init); }

val getGeneralRegVals() { return val(typed_memory_view(32, R)); }
EMSCRIPTEN_BINDINGS(getGeneralRegVals) { function("getGeneralRegVals", &getGeneralRegVals); }

val getFloatRegVals() { return val(typed_memory_view(32, (float *) FPR)); }
EMSCRIPTEN_BINDINGS(getFloatRegVals) { function("getFloatRegVals", &getFloatRegVals); }

val getDoubleRegVals() { return val(typed_memory_view(16, (double *) FPR)); }
EMSCRIPTEN_BINDINGS(getDoubleRegVals) { function("getDoubleRegVals", &getDoubleRegVals); }

val getSpecialRegVals() {
  static int specialRegs[12];
  specialRegs[0] = PC;
  specialRegs[1] = CP0_EPC;
  specialRegs[2] = CP0_Cause;
  specialRegs[3] = CP0_BadVAddr;
  specialRegs[4] = CP0_Status;
  specialRegs[5] = HI;
  specialRegs[6] = LO;
  specialRegs[7] = FIR;
  specialRegs[8] = FCSR;
  specialRegs[9] = FCCR;
  specialRegs[10] = FEXR;
  specialRegs[11] = FENR;

  return val(typed_memory_view(12, specialRegs));
}
EMSCRIPTEN_BINDINGS(getSpecialRegVals) { function("getSpecialRegVals", &getSpecialRegVals); }

static str_stream ss;
std::string getSegment(mem_addr from, mem_addr to) {
  ss_clear(&ss);
  format_insts(&ss, from, to);
  return std::string{ss_to_string(&ss)};
}
EMSCRIPTEN_BINDINGS(getSegment) { function("getSegment", &getSegment); }

std::string getUserText() { return getSegment(TEXT_BOT, text_top); }
EMSCRIPTEN_BINDINGS(getUserText) { function("getUserText", &getUserText); }

std::string getUserData() { return getSegment(DATA_BOT, data_top); }
EMSCRIPTEN_BINDINGS(getUserData) { function("getUserData", &getUserData); }

std::string getUserStack() { return getSegment(ROUND_DOWN(R[29], BYTES_PER_WORD), STACK_TOP); }
EMSCRIPTEN_BINDINGS(getUserStack) { function("getUserStack", &getUserStack); }

std::string getKernelText() { return getSegment(K_TEXT_BOT, k_text_top); }
EMSCRIPTEN_BINDINGS(getKernelText) { function("getKernelText", &getKernelText); }

std::string getKernelData() { return getSegment(K_DATA_BOT, k_data_top); }
EMSCRIPTEN_BINDINGS(getKernelData) { function("getKernelData", &getKernelData); }

extern "C" {
void add_bp(int addr) {
  add_breakpoint(addr);
}

void delete_bp(int addr) {
  delete_breakpoint(addr);
}
}

/* Print an error message. */

void error(char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

/* Print the error message then exit. */

void fatal_error(char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fmt = va_arg(args, char *);
  vfprintf(stderr, fmt, args);
  exit(-1);
}

/* Print an error message and return to top level. */

void run_error(char *fmt, ...) {
  va_list args;

  va_start(args, fmt);

  vfprintf(stderr, fmt, args);
  va_end(args);
}

/* IO facilities: */

void write_output(port fp, char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  fflush(stdout);
  va_end(args);
}

/* Simulate the semantics of fgets (not gets) on Unix file. */

void read_input(char *str, int str_size) {
  char *ptr;

  ptr = str;

  while (1 < str_size) /* Reserve space for null */
  {
    char buf[1];
    if (read((int) console_in.i, buf, 1) <= 0) /* Not in raw mode! */
      break;

    *ptr++ = buf[0];
    str_size -= 1;

    if (buf[0] == '\n') break;
  }

  if (0 < str_size) *ptr = '\0'; /* Null terminate input */
}

int console_input_available() {
  return 0;
}

char get_console_char() {
  char buf;
  read(0, &buf, 1);
  return (buf);
}

void put_console_char(char c) {
  putc(c, stdout);
  fflush(stdout);
}