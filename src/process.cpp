#include <libsdb/process.hpp>
#include <libsdb/bit.hpp>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/personality.h>
#include <libsdb/error.hpp>
#include <libsdb/pipe.hpp>
#include <sys/uio.h>
#include <elf.h>
#include <fstream>
#include <libsdb/target.hpp>

namespace {
    void exit_with_perror(
        sdb::pipe& channel, std::string const& prefix) {
        auto message = prefix + ": " + std::strerror(errno);
        channel.write(
            reinterpret_cast<std::byte*>(message.data()), message.size());
        exit(-1);
    }

    void set_ptrace_options(pid_t pid) {
        if (ptrace(PTRACE_SETOPTIONS, pid, nullptr,
            PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE) < 0) {
            sdb::error::send_errno(
                "Failed to set TRACESYSGOOD and TRACECLONE options");
        }
    }
}

std::unique_ptr<sdb::process>
sdb::process::launch(std::filesystem::path path,
    bool debug,
    std::optional<int> stdout_replacement) {
    pipe channel(/*close_on_exec=*/true);
    pid_t pid;
    if ((pid = fork()) < 0) {
        error::send_errno("fork failed");
    }

    if (pid == 0) {
        if (setpgid(0, 0) < 0) {
            exit_with_perror(channel, "Could not set pgid");
        }
        personality(ADDR_NO_RANDOMIZE);
        channel.close_read();

        if (stdout_replacement) {
            close(STDOUT_FILENO);
            if (dup2(*stdout_replacement, STDOUT_FILENO) < 0) {
                exit_with_perror(channel, "stdout replacement failed");
            }
        }
        if (debug and ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) {
            exit_with_perror(channel, "Tracing failed");
        }
        if (execlp(path.c_str(), path.c_str(), nullptr) < 0) {
            exit_with_perror(channel, "exec failed");
        }
    }

    channel.close_write();
    auto data = channel.read();
    channel.close_read();

    if (data.size() > 0) {
        waitpid(pid, nullptr, 0);
        auto chars = reinterpret_cast<char*>(data.data());
        error::send(std::string(chars, chars + data.size()));
    }

    std::unique_ptr<process> proc(
        new process(pid, /*terminate_on_end=*/true, debug));
    if (debug) {
        proc->wait_on_signal();
        set_ptrace_options(proc->pid());
    }

    return proc;
}

std::unique_ptr<sdb::process>
sdb::process::attach(pid_t pid) {
    if (pid == 0) {
        error::send("Invalid PID");
    }
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
        error::send_errno("Could not attach");
    }

    std::unique_ptr<process> proc(
        new process(pid, /*terminate_on_end=*/false, /*attached=*/true));
    proc->wait_on_signal();
    set_ptrace_options(proc->pid());

    return proc;
}

sdb::process::~process() {
    if (pid_ != 0) {
        int status;
        if (is_attached_) {
            if (state_ == process_state::running) {
                kill(pid_, SIGSTOP);
                waitpid(pid_, &status, 0);
            }
            ptrace(PTRACE_DETACH, pid_, nullptr, nullptr);
            kill(pid_, SIGCONT);
        }

        if (terminate_on_end_) {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
    }
}

sdb::stop_reason sdb::process::step_instruction(std::optional<pid_t> otid) {
    auto tid = otid.value_or(current_thread_);
    std::optional<breakpoint_site*> to_reenable;
    auto pc = get_pc(tid);
    if (breakpoint_sites_.enabled_stoppoint_at_address(pc)) {
        auto& bp = breakpoint_sites_.get_by_address(pc);
        bp.disable();
        to_reenable = &bp;
    }

    swallow_pending_sigstop(tid);
    if (ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr) < 0) {
        error::send_errno("Could not single step");
    }
    auto reason = wait_on_signal(tid);

    if (to_reenable) {
        to_reenable.value()->enable();
    }
    return reason;
}

void sdb::process::step_over_breakpoint(pid_t tid) {
    auto pc = get_pc(tid);
    if (breakpoint_sites_.enabled_stoppoint_at_address(pc)) {
        auto& bp = breakpoint_sites_.get_by_address(pc);
        bp.disable();
        swallow_pending_sigstop(tid);
        if (ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr) < 0) {
            error::send_errno("Failed to single step");
        }
        int wait_status;
        if (waitpid(tid, &wait_status, 0) < 0) {
            error::send_errno("waitpid failed");
        }
        bp.enable();
    }
}

void sdb::process::resume(std::optional<pid_t> otid) {
    auto tid = otid.value_or(current_thread_);
    step_over_breakpoint(tid);
    send_continue(tid);
}

void sdb::process::send_continue(pid_t tid) {
    auto request =
        syscall_catch_policy_.get_mode() == syscall_catch_policy::mode::none ?
        PTRACE_CONT : PTRACE_SYSCALL;
    if (ptrace(request, tid, nullptr, nullptr) < 0) {
        error::send_errno("Could not resume");
    }
    threads_.at(tid).state = process_state::running;
    state_ = process_state::running;
}

sdb::stop_reason::stop_reason(pid_t tid, int wait_status) : tid(tid) {
    if ((wait_status >> 8) == (SIGTRAP | (PTRACE_EVENT_CLONE << 8))) {
        trap_reason = trap_type::clone;
    }
    if (WIFEXITED(wait_status)) {
        reason = process_state::exited;
        info = WEXITSTATUS(wait_status);
    }
    else if (WIFSIGNALED(wait_status)) {
        reason = process_state::terminated;
        info = WTERMSIG(wait_status);
    }
    else if (WIFSTOPPED(wait_status)) {
        reason = process_state::stopped;
        info = WSTOPSIG(wait_status);
    }
}

sdb::stop_reason sdb::process::wait_on_signal(pid_t to_await) {
    int wait_status;
    int options = __WALL;
    pid_t tid;
    if ((tid = waitpid(to_await, &wait_status, options)) < 0) {
        error::send_errno("waitpid failed");
    }
    stop_reason reason(tid, wait_status);
    auto final_reason = handle_signal(reason, true);
    if (!final_reason) {
        resume(tid);
        return wait_on_signal(to_await);
    }
    reason = *final_reason;
    auto& thread = threads_.at(tid);
    thread.reason = reason;
    thread.state = reason.reason;
    if (reason.reason == process_state::exited or
        reason.reason == process_state::terminated) {
        report_thread_lifecycle_event(reason);
        if (tid == pid_) {
            state_ = reason.reason;
            return reason;
        }
        else {
            return wait_on_signal(-1);
        }
    }
    stop_running_threads();
    reason = cleanup_exited_threads(tid).value_or(reason);

    state_ = reason.reason;
    current_thread_ = tid;
    return reason;
}

void sdb::process::read_all_registers(pid_t tid) {
    if (ptrace(PTRACE_GETREGS, tid, nullptr, &get_registers(tid).data_.regs) < 0) {
        error::send_errno("Could not read GPR registers");
    }
    if (ptrace(PTRACE_GETFPREGS, tid, nullptr, &get_registers(tid).data_.i387) < 0) {
        error::send_errno("Could not read FPR registers");
    }
    for (int i = 0; i < 8; ++i) {
        auto id = static_cast<int>(register_id::dr0) + i;
        auto info = register_info_by_id(static_cast<register_id>(id));

        errno = 0;
        std::int64_t data = ptrace(PTRACE_PEEKUSER, tid, info.offset, nullptr);
        if (errno != 0) error::send_errno("Could not read debug register");

        get_registers(tid).data_.u_debugreg[i] = data;
    }
}

sdb::breakpoint_site& sdb::process::create_breakpoint_site(
    virt_addr address, bool hardware, bool internal) {
    if (breakpoint_sites_.contains_address(address)) {
        error::send("Breakpoint site already created at address " +
            std::to_string(address.addr()));
    }
    return breakpoint_sites_.push(
        std::unique_ptr<breakpoint_site>(
            new breakpoint_site(*this, address, hardware, internal)));
}

std::vector<std::byte>
sdb::process::read_memory(virt_addr address, std::size_t amount) const {
    std::vector<std::byte> ret(amount);

    iovec local_desc{ ret.data(), ret.size() };
    std::vector<iovec> remote_descs;
    while (amount > 0) {
        auto up_to_next_page = 0x1000 - (address.addr() & 0xfff);
        auto chunk_size = std::min(amount, up_to_next_page);
        remote_descs.push_back({ reinterpret_cast<void*>(address.addr()), chunk_size });
        amount -= chunk_size;
        address += chunk_size;
    }

    if (process_vm_readv(pid_, &local_desc, /*liovcnt=*/1,
        remote_descs.data(), /*riovcnt=*/remote_descs.size(), /*flags=*/0) < 0) {
        error::send_errno("Could not read process memory");
    }
    return ret;
}

void sdb::process::write_memory(
    virt_addr address, span<const std::byte> data) {
    std::size_t written = 0;
    while (written < data.size()) {
        auto remaining = data.size() - written;
        std::uint64_t word;
        if (remaining >= 8) {
            word = from_bytes<std::uint64_t>(data.begin() + written);
        }
        else {
            auto read = read_memory(address + written, 8);
            auto word_data = reinterpret_cast<char*>(&word);
            std::memcpy(word_data, data.begin() + written, remaining);
            std::memcpy(word_data + remaining, read.data() + remaining, 8 - remaining);
        }
        if (ptrace(PTRACE_POKEDATA, pid_, address + written, word) < 0) {
            error::send_errno("Failed to write memory");
        }
        written += 8;
    }
}

std::vector<std::byte>
sdb::process::read_memory_without_traps(
    virt_addr address, std::size_t amount) const {
    auto memory = read_memory(address, amount);
    auto sites = breakpoint_sites_.get_in_region(
        address, address + amount);
    for (auto site : sites) {
        if (!site->is_enabled() or site->is_hardware()) continue;
        auto offset = site->address() - address.addr();
        memory[offset.addr()] = site->saved_data_;
    }
    return memory;
}

int sdb::process::set_hardware_breakpoint(
    breakpoint_site::id_type id, virt_addr address) {
    return set_hardware_stoppoint(address, stoppoint_mode::execute, 1);
}

namespace {
    std::uint64_t encode_hardware_stoppoint_mode(
        sdb::stoppoint_mode mode) {
        switch (mode) {
        case sdb::stoppoint_mode::write: return 0b01;
        case sdb::stoppoint_mode::read_write: return 0b11;
        case sdb::stoppoint_mode::execute: return 0b00;
        default: sdb::error::send("Invalid stoppoint mode");
        }
    }

    std::uint64_t encode_hardware_stoppoint_size(
        std::size_t size) {
        switch (size) {
        case 1: return 0b00;
        case 2: return 0b01;
        case 4: return 0b11;
        case 8: return 0b10;
        default: sdb::error::send("Invalid stoppoint size");
        }
    }

    int find_free_stoppoint_register(
        std::uint64_t control_register) {
        for (auto i = 0; i < 4; ++i) {
            if ((control_register & (0b11 << (i * 2))) == 0) {
                return i;
            }
        }
        sdb::error::send("No remaining hardware debug registers");
    }
}

int sdb::process::set_hardware_stoppoint(
    virt_addr address, stoppoint_mode mode, std::size_t size) {
    auto& regs = get_registers();
    auto control = regs.read_by_id_as<std::uint64_t>(
        register_id::dr7);

    int free_space = find_free_stoppoint_register(control);

    auto id = static_cast<int>(register_id::dr0) + free_space;
    regs.write_by_id(static_cast<register_id>(id), address.addr());

    auto mode_flag = encode_hardware_stoppoint_mode(mode);
    auto size_flag = encode_hardware_stoppoint_size(size);

    auto enable_bit = (1 << (free_space * 2));
    auto mode_bits = (mode_flag << (free_space * 4 + 16));
    auto size_bits = (size_flag << (free_space * 4 + 18));

    auto clear_mask = (0b11 << (free_space * 2)) |
        (0b1111 << (free_space * 4 + 16));
    auto masked = control & ~clear_mask;

    masked |= enable_bit | mode_bits | size_bits;

    regs.write_by_id(register_id::dr7, masked);

    for (auto& [tid, _] : threads_) {
        if (tid == current_thread_) continue;
        auto& other_regs = get_registers(tid);
        other_regs.write_by_id(
            static_cast<register_id>(id), address.addr());
        other_regs.write_by_id(register_id::dr7, masked);
    }

    return free_space;
}

void sdb::process::clear_hardware_stoppoint(int index) {
    auto id = static_cast<int>(register_id::dr0) + index;
    get_registers().write_by_id(static_cast<register_id>(id), 0);

    auto control = get_registers().
        read_by_id_as<std::uint64_t>(register_id::dr7);

    auto clear_mask = (0b11 << (index * 2))
        | (0b1111 << (index * 4 + 16));
    auto masked = control & ~clear_mask;

    get_registers().write_by_id(register_id::dr7, masked);

    for (auto& [tid, _] : threads_) {
        if (tid == current_thread_) continue;
        auto& other_regs = get_registers(tid);
        other_regs.write_by_id(static_cast<register_id>(id), 0);
        other_regs.write_by_id(register_id::dr7, masked);
    }
}

int sdb::process::set_watchpoint(
    watchpoint::id_type id, virt_addr address,
    stoppoint_mode mode, std::size_t size) {
    return set_hardware_stoppoint(address, mode, size);
}

sdb::watchpoint&
sdb::process::create_watchpoint(virt_addr address, stoppoint_mode mode, std::size_t size) {
    if (watchpoints_.contains_address(address)) {
        error::send("Watchpoint already created at address " +
            std::to_string(address.addr()));
    }
    return watchpoints_.push(
        std::unique_ptr<watchpoint>(new watchpoint(*this, address, mode, size)));
}

void sdb::process::augment_stop_reason(sdb::stop_reason& reason) {
    auto tid = reason.tid;
    siginfo_t info;
    if (ptrace(PTRACE_GETSIGINFO, tid, nullptr, &info) < 0) {
        error::send_errno("Failed to get signal info");
    }

    if (reason.info == (SIGTRAP | 0x80) or info.si_code == TRAP_BRKPT) {
        auto& sys_info = reason.syscall_info.emplace();
        auto& regs = get_registers(tid);

        if (expecting_syscall_exit_) {
            sys_info.entry = false;
            sys_info.id = regs.read_by_id_as<std::uint64_t>(
                register_id::orig_rax);
            sys_info.ret = regs.read_by_id_as<std::uint64_t>(
                register_id::rax);
            expecting_syscall_exit_ = false;
        }
        else {
            sys_info.entry = true;
            sys_info.id = regs.read_by_id_as<std::uint64_t>(
                register_id::orig_rax);

            std::array<register_id, 6> arg_regs = {
                register_id::rdi, register_id::rsi, register_id::rdx,
                register_id::r10, register_id::r8, register_id::r9
            };
            for (auto i = 0; i < 6; ++i) {
                sys_info.args[i] = regs.read_by_id_as<std::uint64_t>(
                    arg_regs[i]);
            }

            expecting_syscall_exit_ = true;
        }

        reason.info = SIGTRAP;
        reason.trap_reason = trap_type::syscall;
        return;
    }

    expecting_syscall_exit_ = false;

    reason.trap_reason = trap_type::unknown;
    if (reason.info == SIGTRAP) {
        switch (info.si_code) {
        case TRAP_TRACE:
            reason.trap_reason = trap_type::single_step;
            break;
        case SI_KERNEL:
            reason.trap_reason = trap_type::software_break;
            break;
        case TRAP_HWBKPT:
            reason.trap_reason = trap_type::hardware_break;
            break;
        }
    }
}

std::variant<sdb::breakpoint_site::id_type, sdb::watchpoint::id_type>
sdb::process::get_current_hardware_stoppoint(std::optional<pid_t> otid) const {
    auto& regs = get_registers(otid);
    auto status = regs.read_by_id_as<std::uint64_t>(register_id::dr6);
    auto index = __builtin_ctzll(status);

    auto id = static_cast<int>(register_id::dr0) + index;
    auto addr = virt_addr(
        regs.read_by_id_as<std::uint64_t>(static_cast<register_id>(id)));

    using ret = std::variant<sdb::breakpoint_site::id_type, sdb::watchpoint::id_type>;
    if (breakpoint_sites_.contains_address(addr)) {
        auto site_id = breakpoint_sites_.get_by_address(addr).id();
        return ret{ std::in_place_index<0>, site_id };
    }
    else {
        auto watch_id = watchpoints_.get_by_address(addr).id();
        return ret{ std::in_place_index<1>, watch_id };
    }
}

bool sdb::process::should_resume_from_syscall(
    const stop_reason& reason) {
    if (syscall_catch_policy_.get_mode() ==
        syscall_catch_policy::mode::some) {
        auto& to_catch = syscall_catch_policy_.get_to_catch();
        auto found = std::find(
            begin(to_catch), end(to_catch), reason.syscall_info->id);

        if (found == end(to_catch)) {
            return true;
        }
    }

    return false;
}

std::unordered_map<int, std::uint64_t> sdb::process::get_auxv() const {
    auto path = "/proc/" + std::to_string(pid_) + "/auxv";
    std::ifstream auxv(path);

    std::unordered_map<int, std::uint64_t> ret;
    std::uint64_t id, value;

    auto read = [&](auto& into) {
        auxv.read(reinterpret_cast<char*>(&into), sizeof(into));
        };

    for (read(id); id != AT_NULL; read(id)) {
        read(value);
        ret[id] = value;
    }
    return ret;
}

sdb::breakpoint_site&
sdb::process::create_breakpoint_site(
    breakpoint* parent, breakpoint_site::id_type id, virt_addr address,
    bool hardware, bool internal) {
    if (breakpoint_sites_.contains_address(address)) {
        error::send("Breakpoint site already created at address " +
            std::to_string(address.addr()));
    }
    return breakpoint_sites_.push(
        std::unique_ptr<breakpoint_site>(
            new breakpoint_site(
                parent, id, *this, address, hardware, internal)));
}

void sdb::process::populate_existing_threads() {
    auto path = "/proc/" + std::to_string(pid_) + "/task";
    for (auto& entry : std::filesystem::directory_iterator(path)) {
        auto tid = std::stoi(entry.path().filename().string());
        threads_.emplace(tid, thread_state{ tid, registers(*this, tid) });
    }
}

sdb::virt_addr sdb::process::get_pc(std::optional<pid_t> otid) const {
    return virt_addr{
        get_registers(otid).read_by_id_as<std::uint64_t>(register_id::rip)
    };
}

void sdb::process::set_pc(
    virt_addr address, std::optional<pid_t> otid) {
    get_registers(otid).write_by_id(register_id::rip, address.addr());
}

sdb::registers& sdb::process::get_registers(
    std::optional<pid_t> otid) {
    auto tid = otid.value_or(current_thread_);
    return threads_.at(tid).regs;
}

const sdb::registers& sdb::process::get_registers(
    std::optional<pid_t> otid) const {
    return const_cast<process*>(this)->get_registers(otid);
}

void sdb::process::write_user_area(
    std::size_t offset, std::uint64_t data, std::optional<pid_t> otid) {
    auto tid = otid.value_or(current_thread_);
    if (ptrace(PTRACE_POKEUSER, tid, offset, data) < 0) {
        error::send_errno("Could not write to user area");
    }
}

void sdb::process::write_fprs(
    const user_fpregs_struct& fprs, std::optional<pid_t> otid) {
    auto tid = otid.value_or(current_thread_);
    if (ptrace(PTRACE_SETFPREGS, tid, nullptr, &fprs) < 0) {
        error::send_errno("Could not write floating point registers");
    }
}

void sdb::process::write_gprs(const user_regs_struct& gprs, std::optional<pid_t> otid) {
    auto tid = otid.value_or(current_thread_);
    if (ptrace(PTRACE_SETREGS, tid, nullptr, &gprs) < 0) {
        error::send_errno("Could not write general purpose registers");
    }
}

void sdb::process::resume_all_threads() {
    for (auto& [tid, _] : threads_) {
        step_over_breakpoint(tid);
    }
    for (auto& [tid, _] : threads_) {
        send_continue(tid);
    }
}

void sdb::process::stop_running_threads() {
    for (auto& [tid, thread] : threads_) {
        if (thread.state == process_state::running) {
            if (!thread.pending_sigstop) {
                tgkill(pid_, tid, SIGSTOP);
            }

            int wait_status;
            waitpid(tid, &wait_status, 0);

            stop_reason thread_reason(tid, wait_status);
            if (thread_reason.reason == process_state::stopped) {
                if (thread_reason.info != SIGSTOP) {
                    thread.pending_sigstop = true;
                }
                else if (thread.pending_sigstop) {
                    thread.pending_sigstop = false;
                }
            }

            thread_reason = handle_signal(thread_reason, false).value_or(thread_reason);
            threads_.at(tid).reason = thread_reason;
            threads_.at(tid).state = thread_reason.reason;
        }
    }
}

std::optional<sdb::stop_reason> sdb::process::cleanup_exited_threads(pid_t main_stop_tid) {
    std::vector<pid_t> to_remove;
    std::optional<stop_reason> to_report;
    for (auto& [tid, thread] : threads_) {
        if (tid != main_stop_tid and
            (thread.state == process_state::exited or
                thread.state == process_state::terminated)) {
            report_thread_lifecycle_event(thread.reason);
            to_remove.push_back(tid);
            if (tid == pid_) {
                to_report = thread.reason;
            }
        }
    }

    for (auto tid : to_remove) {
        threads_.erase(tid);
    }
    return to_report;
}

void sdb::process::report_thread_lifecycle_event(
    const sdb::stop_reason& reason) {
    if (thread_lifecycle_callback_) {
        thread_lifecycle_callback_(reason);
    }
    if (target_) {
        target_->notify_thread_lifecycle_event(reason);
    }
}

std::optional<sdb::stop_reason> sdb::process::handle_signal(
    stop_reason reason, bool is_main_stop) {
    auto tid = reason.tid;

    if (reason.trap_reason and
        *reason.trap_reason == trap_type::clone and
        is_main_stop) {
        return std::nullopt;
    }
    if (is_attached_ and reason.reason == process_state::stopped) {
        if (!threads_.count(tid)) {
            threads_.emplace(tid, thread_state{ tid, registers(*this, tid) });
            report_thread_lifecycle_event(reason);
            if (is_main_stop) {
                return std::nullopt;
            }
        }
        if (threads_.at(tid).pending_sigstop and reason.info == SIGSTOP) {
            threads_.at(tid).pending_sigstop = false;
            return std::nullopt;
        }

        read_all_registers(tid);
        augment_stop_reason(reason);
        if (reason.info == SIGTRAP) {
            auto instr_begin = get_pc(tid) - 1;
            if (reason.trap_reason == trap_type::software_break and
                breakpoint_sites_.contains_address(instr_begin) and
                breakpoint_sites_.get_by_address(instr_begin).is_enabled()) {
                set_pc(instr_begin, tid);

                auto& bp = breakpoint_sites_.get_by_address(instr_begin);
                if (bp.parent_) {
                    bool should_restart = bp.parent_->notify_hit();
                    if (should_restart and is_main_stop) {
                        return std::nullopt;
                    }
                }
            }
            else if (reason.trap_reason == trap_type::hardware_break) {
                auto id = get_current_hardware_stoppoint(tid);
                if (id.index() == 1) {
                    watchpoints_.get_by_id(std::get<1>(id)).update_data();
                }
            }
            else if (reason.trap_reason == trap_type::syscall and
                is_main_stop and
                should_resume_from_syscall(reason)) {
                return std::nullopt;
            }
        }
        if (target_) target_->notify_stop(reason);
    }
    return reason;
}

void sdb::process::swallow_pending_sigstop(pid_t tid) {
    if (threads_.at(tid).pending_sigstop) {
        ptrace(PTRACE_CONT, tid, nullptr, nullptr);
        waitpid(tid, nullptr, 0);
        threads_.at(tid).pending_sigstop = false;
    }
}

std::string sdb::process::read_string(virt_addr address) const {
    std::string ret;
    while (true) {
        auto data = read_memory(address, 1024);
        for (auto c : data) {
            if (c == std::byte{ 0 }) return ret;
            ret.push_back(static_cast<char>(c));
        }
    }
}

sdb::registers sdb::process::inferior_call(
    sdb::virt_addr func_addr, sdb::virt_addr return_addr,
    const sdb::registers& regs_to_restore, std::optional<pid_t> otid) {
    auto tid = otid.value_or(current_thread_);
    auto& regs = get_registers(tid);

    regs.write_by_id(sdb::register_id::rip, func_addr.addr(), true);

    auto rsp = regs.read_by_id_as<std::uint64_t>(sdb::register_id::rsp);

    rsp -= 8;
    write_memory(
        sdb::virt_addr{ rsp }, sdb::to_byte_span(return_addr.addr()));
    regs.write_by_id(sdb::register_id::rsp, rsp, true);

    resume(tid);
    auto reason = wait_on_signal(tid);
    if (reason.reason != sdb::process_state::stopped) {
        sdb::error::send("Function call failed");
    }

    auto new_regs = regs;
    regs = regs_to_restore;
    regs.flush();
    if (target_) target_->notify_stop(reason);

    return new_regs;
}