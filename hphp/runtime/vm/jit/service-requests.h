/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2015 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#ifndef incl_HPHP_RUNTIME_VM_SERVICE_REQUESTS_H_
#define incl_HPHP_RUNTIME_VM_SERVICE_REQUESTS_H_

#include "hphp/runtime/base/types.h"
#include "hphp/runtime/vm/srckey.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/data-block.h"

#include <folly/Optional.h>

namespace HPHP {

struct ActRec;

namespace jit {

///////////////////////////////////////////////////////////////////////////////

/*
 * A service request is a co-routine used to invoke JIT translation services.
 *
 * Most service requests are accomplished by jumping to a service request stub,
 * which rolls up arguments and calls into the JIT via handleSRHelper().
 * Afterwards, we re-enter the TC, returning control non-locally to the next
 * logical instruction.
 *
 * The implementation of the service request will return the TCA at which to
 * resume execution even if it's another stub rather than the calling function.
 */
#define SERVICE_REQUESTS  \
  /*
   * bind_jmp(TCA jmp, SrcKey target, TransFlags trflags)
   *
   * A jmp to the potentially untranslated code for `target'.
   *
   * Jump to a service request stub, which invokes the JIT and looks up the
   * translation for `target'---or creates a translation if one does not exist.
   * The address of the translation is then smashed into the immediate of the
   * jump instruction (whose address is passed via `jmp').
   */                     \
  REQ(BIND_JMP)           \
                          \
  /*
   * bind_addr(TCA* addr, SrcKey target, TransFlags trflags)
   *
   * A code pointer to the potentially untranslated `target'; used for
   * just-in-time indirect call translations.
   *
   * Similar to bind_jmp, except that the smash target is *addr instead of the
   * jmp instruction's immediate.  When we emit a bind_addr, we only emit the
   * request stub and store its address to *addr; someone else has to emit the
   * indirect jump that actually invokes the service request.
   */                     \
  REQ(BIND_ADDR)          \
                          \
  /*
   * bind_jcc_first(TCA jcc, SrcKey taken, SrcKey next, bool did_take)
   *
   * A branch between two potentially untranslated targets.
   *
   * A bind_jcc_1st is emitted as a jcc followed by a jmp, both to the same
   * stub.  When invoked, a translation of the appropriate side of the branch
   * (indicated by `did_take') is obtained, and the jcc is rewritten so that it
   * will translate the other side of the branch when the inverse condition is
   * met.
   *
   * @see: MCGenerator::bindJccFirst()
   */                     \
  REQ(BIND_JCC_FIRST)     \
                          \
  /*
   * retranslate(Offset off, TransFlags trflags)
   *
   * A request to retranslate the current function at bytecode offset `off',
   * for when no existing translations support the incoming types.
   *
   * The smash target(s) of a retranslate is stored in
   * SrcRec::m_tailFallbackJumps.
   */                     \
  REQ(RETRANSLATE)        \
                          \
  /*
   * retranslate_opt(SrcKey target, TransID transID)
   *
   * A request to retranslate a function entry `target', leveraging profiling
   * data to produce a larger, more optimized translation.  Only used when PGO
   * is enabled.
   */                     \
  REQ(RETRANSLATE_OPT)    \
                          \
  /*
   * post_interp_ret(ActRec* arg, ActRec* caller)
   * post_debugger_ret()
   *
   * post_interp_ret is invoked in the case that translated code in the TC
   * executes the return for a frame that was pushed by the interpreter---since
   * there is no TCA to return to.
   *
   * post_debugger_ret is a similar request that is used when translated code
   * returns from a frame that had its saved return address smashed by the
   * debugger.
   */                     \
  REQ(POST_INTERP_RET)    \
  REQ(POST_DEBUGGER_RET)

/*
 * Service request types.
 */
enum ServiceRequest {
#define REQ(nm) REQ_##nm,
  SERVICE_REQUESTS
#undef REQ
};

///////////////////////////////////////////////////////////////////////////////

namespace svcreq {

///////////////////////////////////////////////////////////////////////////////

/*
 * ID to name mapping for tracing.
 */
inline const char* to_name(ServiceRequest req) {
  static const char* reqNames[] = {
#define REQ(nm) #nm,
    SERVICE_REQUESTS
#undef REQ
  };
  return reqNames[req];
}

#undef SERVICE_REQUESTS

///////////////////////////////////////////////////////////////////////////////
// Emitters.

/*
 * Type-discriminated service request argument.
 *
 * Each argument is written to a register when the service request is made:
 *  - Immediates are loaded directly.
 *  - Addresses are loaded using an rip-relative lea to aid code relocation.
 *  - Condition codes produce a setcc based on the status flags passed to the
 *    emit routine.
 */
struct Arg {
  enum class Kind { Immed, Address, CondCode };

  explicit Arg(uint64_t imm) : kind{Kind::Immed}, imm{imm} {}
  explicit Arg(TCA addr) : kind{Kind::Address}, addr{addr} {}
  explicit Arg(ConditionCode cc) : kind{Kind::CondCode}, cc{cc} {}

public:
  Kind kind;
  union {
    uint64_t imm;
    TCA addr;
    ConditionCode cc;
  };
};

using ArgVec = jit::vector<Arg>;

/*
 * Service request stub emitters.
 *
 * Some clients (e.g., unique stubs, bind_jcc_1st machinery) need to emit stubs
 * directly without the supporting code (e.g., smashable jumps).
 *
 * Service request stubs can be either persistent or ephemeral.  The only
 * difference (besides that ephemeral service requests require a stub start
 * address) is that ephemeral requests are padded to stub_size().
 */
template<typename... Args>
TCA emit_persistent(CodeBlock& cb,
                    folly::Optional<FPInvOffset> spOff,
                    ServiceRequest sr,
                    Args... args);
template<typename... Args>
TCA emit_ephemeral(CodeBlock& cb,
                   TCA start,
                   folly::Optional<FPInvOffset> spOff,
                   ServiceRequest sr,
                   Args... args);

/*
 * Space used by an ephemeral stub.
 *
 * All ephemeral service request stubs are sized to this fixed, architecture-
 * dependent size, which is guaranteed to fit all service request types along
 * with a terminal padding instruction.
 */
size_t stub_size();

/*
 * Extract the VM stack offset associated with a service request stub.
 *
 * When we emit service requests stubs for non-resumed TC contexts, we first
 * emit code that rematerializes the VM stack pointer.  Sometimes, we want to
 * replace a stub with a different service request (e.g., bind_jcc_1st), so we
 * have to fish the offset out of the stub first.
 */
FPInvOffset extract_spoff(TCA stub);

///////////////////////////////////////////////////////////////////////////////

/*
 * Maximum number of arguments a service request can accept.
 */
constexpr int kMaxArgs = 4;

/*
 * Service request metadata.
 *
 * This structure is created on the stack by handleSRHelper() from the SR
 * arguments passed in registers.  Any changes to its size or layout of must be
 * reflected in handleSRHelper() in translator-asm-helpers.S.
 */
struct ReqInfo {
  /*
   * The service request type.
   */
  ServiceRequest req;

  /*
   * Address of the service request's code stub for non-persistent requests.
   *
   * The service request handler will free this stub if it's set, after the
   * service is performed.
   */
  TCA stub;

  /*
   * Service request arguments; see SERVICE_REQUESTS for documentation.
   */
  union {
    TCA tca;
    Offset offset;
    SrcKey::AtomicInt sk;
    TransFlags trflags;
    TransID transID;
    bool boolVal;
    ActRec* ar;
  } args[kMaxArgs];
};

static_assert(sizeof(ReqInfo) == 0x30,
              "rsp adjustments in handleSRHelper");

///////////////////////////////////////////////////////////////////////////////

} // svcreq

///////////////////////////////////////////////////////////////////////////////

/*
 * Service request assembly stub.
 *
 * Called by translated code before a service request to pack argument
 * registers into a ReqInfo and perform some other bookkeeping tasks.
 */
extern "C" void handleSRHelper();

///////////////////////////////////////////////////////////////////////////////

/*
 * A REQ_BIND_JMP service request passes an address of a jump that can be
 * patched.  This function lets you change this jump address for an existing
 * REQ_BIND_JMP stub to `newJmpIp'.  The caller must indicate whether the stub
 * was created with a target SrcKey that is a resumed function.
 *
 * Pre: the `stub' must be a REQ_BIND_JMP stub.
 */
void adjustBindJmpPatchableJmpAddress(TCA stub,
                                      bool targetIsResumed,
                                      TCA newJmpIp);

///////////////////////////////////////////////////////////////////////////////

}}

#include "hphp/runtime/vm/jit/service-requests-inl.h"

#endif
