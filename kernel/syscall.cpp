#include "syscall.hpp"

#include <array>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <cstring>

#include "asmfunc.h"
#include "msr.hpp"
#include "logger.hpp"
#include "task.hpp"
#include "terminal.hpp"
#include "font.hpp"
#include "timer.hpp"
#include "keyboard.hpp"
#include "app_event.hpp"

namespace syscall {
  struct Result {
    uint64_t value;
    int error;
  };

#define SYSCALL(name) \
  Result name( \
      uint64_t arg1, uint64_t arg2, uint64_t arg3, \
      uint64_t arg4, uint64_t arg5, uint64_t arg6)

SYSCALL(LogString) {
  if (arg1 != kError && arg1 != kWarn && arg1 != kInfo && arg1 != kDebug) {
    return { 0, EPERM };
  }
  const char* s = reinterpret_cast<const char*>(arg2);
  const auto len = strlen(s);
  if (len > 1024) {
    return { 0, E2BIG };
  }
  Log(static_cast<LogLevel>(arg1), "%s", s);
  return { len, 0 };
}

SYSCALL(PutString) {
  const auto fd = arg1;
  const char* s = reinterpret_cast<const char*>(arg2);
  const auto len = arg3;
  if (len > 1024) {
    return { 0, E2BIG };
  }

  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");

  if (fd < 0 || task.Files().size() <= fd || !task.Files()[fd]) {
    return { 0, EBADF };
  }
  return { task.Files()[fd]->Write(s, len), 0 };
}

SYSCALL(Exit) {
  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");
  return { task.OSStackPointer(), static_cast<int>(arg1) };
}

SYSCALL(OpenWindow) {
  const int w = arg1, h = arg2, x = arg3, y = arg4;
  const auto title = reinterpret_cast<const char*>(arg5);
  const auto win = std::make_shared<ToplevelWindow>(
      w, h, screen_config.pixel_format, title);

  __asm__("cli");
  const auto layer_id = layer_manager->NewLayer()
    .SetWindow(win)
    .SetDraggable(true)
    .Move({x, y})
    .ID();
  active_layer->Activate(layer_id);

  const auto task_id = task_manager->CurrentTask().ID();
  layer_task_map->insert(std::make_pair(layer_id, task_id));
  __asm__("sti");

  return { layer_id, 0 };
}

namespace {
  template <class Func, class... Args>
  Result DoWinFunc(Func f, uint64_t layer_id_flags, Args... args) {
    const uint32_t layer_flags = layer_id_flags >> 32;
    const unsigned int layer_id = layer_id_flags & 0xffffffff;

    __asm__("cli");
    auto layer = layer_manager->FindLayer(layer_id);
    __asm__("sti");
    if (layer == nullptr) {
      return { 0, EBADF };
    }

    const auto res = f(*layer->GetWindow(), args...);
    if (res.error) {
      return res;
    }

    if ((layer_flags & 1) == 0) {
      __asm__("cli");
      layer_manager->Draw(layer_id);
      __asm__("sti");
    }

    return res;
  }
}

SYSCALL(WinWriteString) {
  return DoWinFunc(
      [](Window& win,
         int x, int y, uint32_t color, const char* s) {
        WriteString(*win.Writer(), {x, y}, s, ToColor(color));
        return Result{ 0, 0 };
      }, arg1, arg2, arg3, arg4, reinterpret_cast<const char*>(arg5));
}

SYSCALL(WinFillRectangle) {
  return DoWinFunc(
      [](Window& win,
         int x, int y, int w, int h, uint32_t color) {
        FillRectangle(*win.Writer(), {x, y}, {w, h}, ToColor(color));
        return Result{ 0, 0 };
      }, arg1, arg2, arg3, arg4, arg5, arg6);
}

SYSCALL(GetCurrentTick) {
  return { timer_manager->CurrentTick(), kTimerFreq };
}

SYSCALL(WinRedraw) {
  return DoWinFunc(
      [](Window&) {
        return Result{ 0, 0 };
      }, arg1);
}

SYSCALL(WinDrawLine) {
  return DoWinFunc(
      [](Window& win,
         int x0, int y0, int x1, int y1, uint32_t color) {
        auto sign = [](int x) {
          return (x > 0) ? 1 : (x < 0) ? -1 : 0;
        };
        const int dx = x1 - x0 + sign(x1 - x0);
        const int dy = y1 - y0 + sign(y1 - y0);

        if (dx == 0 && dy == 0) {
          win.Writer()->Write({x0, y0}, ToColor(color));
          return Result{ 0, 0 };
        }

        const auto floord = static_cast<double(*)(double)>(floor);
        const auto ceild = static_cast<double(*)(double)>(ceil);

        if (abs(dx) >= abs(dy)) {
          if (dx < 0) {
            std::swap(x0, x1);
            std::swap(y0, y1);
          }
          const auto roundish = y1 >= y0 ? floord : ceild;
          const double m = static_cast<double>(dy) / dx;
          for (int x = x0; x <= x1; ++x) {
            const int y = roundish(m * (x - x0) + y0);
            win.Writer()->Write({x, y}, ToColor(color));
          }
        } else {
          if (dy < 0) {
            std::swap(x0, x1);
            std::swap(y0, y1);
          }
          const auto roundish = x1 >= x0 ? floord : ceild;
          const double m = static_cast<double>(dx) / dy;
          for (int y = y0; y <= y1; ++y) {
            const int x = roundish(m * (y - y0) + x0);
            win.Writer()->Write({x, y}, ToColor(color));
          }
        }
        return Result{ 0, 0 };
      }, arg1, arg2, arg3, arg4, arg5, arg6);
}

SYSCALL(CloseWindow) {
  const unsigned int layer_id = arg1 & 0xffffffff;
  const auto err = CloseLayer(layer_id);
  if (err.Cause() == Error::kNoSuchEntry) {
    return { EBADF, 0 };
  }
  return { 0, 0 };
}

SYSCALL(ReadEvent) {
  if (arg1 < 0x8000'0000'0000'0000) {
    return { 0, EFAULT };
  }
  const auto app_events = reinterpret_cast<AppEvent*>(arg1);
  const size_t len = arg2;

  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");
  size_t i = 0;

  while (i < len) {
    __asm__("cli");
    auto msg = task.ReceiveMessage();
    if (!msg && i == 0) {
      task.Sleep();
      continue;
    }
    __asm__("sti");

    if (!msg) {
      break;
    }

    switch (msg->type) {
    case Message::kKeyPush:
      if (msg->arg.keyboard.keycode == 20 /* Q key */ &&
          msg->arg.keyboard.modifier & (kLControlBitMask | kRControlBitMask)) {
        app_events[i].type = AppEvent::kQuit;
        ++i;
      } else {
        app_events[i].type = AppEvent::kKeyPush;
        app_events[i].arg.keypush.modifier = msg->arg.keyboard.modifier;
        app_events[i].arg.keypush.keycode = msg->arg.keyboard.keycode;
        app_events[i].arg.keypush.ascii = msg->arg.keyboard.ascii;
        app_events[i].arg.keypush.press = msg->arg.keyboard.press;
        ++i;
      }
      break;
    case Message::kMouseMove:
      app_events[i].type = AppEvent::kMouseMove;
      app_events[i].arg.mouse_move.x = msg->arg.mouse_move.x;
      app_events[i].arg.mouse_move.y = msg->arg.mouse_move.y;
      app_events[i].arg.mouse_move.dx = msg->arg.mouse_move.dx;
      app_events[i].arg.mouse_move.dy = msg->arg.mouse_move.dy;
      app_events[i].arg.mouse_move.buttons = msg->arg.mouse_move.buttons;
      ++i;
      break;
    case Message::kMouseButton:
      app_events[i].type = AppEvent::kMouseButton;
      app_events[i].arg.mouse_button.x = msg->arg.mouse_button.x;
      app_events[i].arg.mouse_button.y = msg->arg.mouse_button.y;
      app_events[i].arg.mouse_button.press = msg->arg.mouse_button.press;
      app_events[i].arg.mouse_button.button = msg->arg.mouse_button.button;
      ++i;
      break;
    case Message::kTimerTimeout:
      if (msg->arg.timer.value < 0) {
        app_events[i].type = AppEvent::kTimerTimeout;
        app_events[i].arg.timer.timeout = msg->arg.timer.timeout;
        app_events[i].arg.timer.value = -msg->arg.timer.value;
        ++i;
      }
      break;
    case Message::kWindowClose:
      app_events[i].type = AppEvent::kQuit;
      ++i;
      break;
    default:
      Log(kInfo, "uncaught event type: %u\n", msg->type);
    }
  }

  return { i, 0 };
}

SYSCALL(CreateTimer) {
  const unsigned int mode = arg1;
  const int timer_value = arg2;
  if (timer_value <= 0) {
    return { 0, EINVAL };
  }

  __asm__("cli");
  const uint64_t task_id = task_manager->CurrentTask().ID();
  __asm__("sti");

  unsigned long timeout = arg3 * kTimerFreq / 1000;
  if (mode & 1) { // relative
    timeout += timer_manager->CurrentTick();
  }

  __asm__("cli");
  timer_manager->AddTimer(Timer{timeout, -timer_value, task_id});
  __asm__("sti");
  return { timeout * 1000 / kTimerFreq, 0 };
}

namespace {
  size_t AllocateFD(Task& task) {
    const size_t num_files = task.Files().size();
    for (size_t i = 0; i < num_files; ++i) {
      if (!task.Files()[i]) {
        return i;
      }
    }
    task.Files().emplace_back();
    return num_files;
  }

  std::pair<fat::DirectoryEntry*, int> CreateFile(const char* path) {
    auto [ file, err ] = fat::CreateFile(path);
    switch (err.Cause()) {
    case Error::kIsDirectory: return { file, EISDIR };
    case Error::kNoSuchEntry: return { file, ENOENT };
    case Error::kNoEnoughMemory: return { file, ENOSPC };
    default: return { file, 0 };
    }
  }
} // namespace

SYSCALL(OpenFile) {
  const char* path = reinterpret_cast<const char*>(arg1);
  const int flags = arg2;
  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");

  if (strcmp(path, "@stdin") == 0) {
    return { 0, 0 };
  }

  auto [ file, post_slash ] = fat::FindFile(path);
  if (file == nullptr) {
    if ((flags & O_CREAT) == 0) {
      return { 0, ENOENT };
    }
    auto [ new_file, err ] = CreateFile(path);
    if (err) {
      return { 0, err };
    }
    file = new_file;
  } else if (file->attr != fat::Attribute::kDirectory && post_slash) {
    return { 0, ENOENT };
  }

  size_t fd = AllocateFD(task);
  task.Files()[fd] = std::make_unique<fat::FileDescriptor>(*file);
  return { fd, 0 };
}

SYSCALL(ReadFile) {
  const int fd = arg1;
  void* buf = reinterpret_cast<void*>(arg2);
  size_t count = arg3;
  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");

  if (fd < 0 || task.Files().size() <= fd || !task.Files()[fd]) {
    return { 0, EBADF };
  }
  return { task.Files()[fd]->Read(buf, count), 0 };
}

SYSCALL(DemandPages) {
  const size_t num_pages = arg1;
  // const int flags = arg2;
  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");

  const uint64_t dp_end = task.DPagingEnd();
  task.SetDPagingEnd(dp_end + 4096 * num_pages);
  return { dp_end, 0 };
}

SYSCALL(MapFile) {
  const int fd = arg1;
  size_t* file_size = reinterpret_cast<size_t*>(arg2);
  // const int flags = arg3;
  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");

  if (fd < 0 || task.Files().size() <= fd || !task.Files()[fd]) {
    return { 0, EBADF };
  }

  *file_size = task.Files()[fd]->Size();
  const uint64_t vaddr_end = task.FileMapEnd();
  const uint64_t vaddr_begin = (vaddr_end - *file_size) & 0xffff'ffff'ffff'f000;
  task.SetFileMapEnd(vaddr_begin);
  task.FileMaps().push_back(FileMapping{fd, vaddr_begin, vaddr_end});
  return { vaddr_begin, 0 };
}

SYSCALL(IsTerminal) {
  const int fd = arg1;
  __asm__("cli");
  auto& task = task_manager->CurrentTask();
  __asm__("sti");

  if (fd < 0 || task.Files().size() <= fd || !task.Files()[fd]) {
    return { 0, EBADF };
  }

  return { task.Files()[fd]->IsTerminal(), 0 };
}

// Linux System call
SYSCALL(read) {
  if (arg1 != kError && arg1 != kWarn && arg1 != kInfo && arg1 != kDebug) {
    return { 0, EPERM };
  }
  const char* s = reinterpret_cast<const char*>(arg2);
  const auto len = strlen(s);
  if (len > 1024) {
    return { 0, E2BIG };
  }
  Log(static_cast<LogLevel>(arg1), "%s", s);
  return { len, 0 };
}

SYSCALL(write) {
  if (arg1 != kError && arg1 != kWarn && arg1 != kInfo && arg1 != kDebug) {
    return { 0, EPERM };
  }
  const char* s = reinterpret_cast<const char*>(arg2);
  const auto len = strlen(s);
  if (len > 1024) {
    return { 0, E2BIG };
  }
  Log(static_cast<LogLevel>(arg1), "%s", s);
  return { len, 0 };
}


SYSCALL(open) {
  if (arg1 != kError && arg1 != kWarn && arg1 != kInfo && arg1 != kDebug) {
    return { 0, EPERM };
  }
  const char* s = reinterpret_cast<const char*>(arg2);
  const auto len = strlen(s);
  if (len > 1024) {
    return { 0, E2BIG };
  }
  Log(static_cast<LogLevel>(arg1), "%s", s);
  return { len, 0 };
}

SYSCALL(getuid) {
  return { 0, 0 };
}

SYSCALL(geteuid) {
  return { 0, 0 };
}

SYSCALL(getegid) {
  return { 0, 0 };
}

SYSCALL(getgid) {
  return { 0, 0 };
}

SYSCALL(dummy) {
  unsigned int syscallNum = getEAX();
  const char *msg1 = "Error: Invalid Syscall Number\n";
  char s[100];
  int length = std::sprintf(s, "There is no Syscall Number: 0x%08X\n", syscallNum);
  syscall::PutString(1, (uint64_t)msg1, strlen(msg1), 1, 1, 1);
  syscall::PutString(1, (uint64_t)s, length, 1, 1, 1);
  while (true) __asm__("hlt");
  return { 0, 0 };
}

#undef SYSCALL

} // namespace syscall

extern "C" syscall::Result invalid_Syscall_num(unsigned int syscallNum){
  const char *msg1 = "Error: Invalid Syscall Number\n";
  char s[100]; // スタック上
  // 他の方法としてnew char[100], malloc(100) ヒープ領域に生成
  // 場所は違うけど、どちらも100byteの配列が作られる
  int length = std::sprintf(s, "There is no Syscall Number: 0x%08X\n", syscallNum);
  // PUtString/Writeは書き込むべきbyte数 -> null文字は含まない
  syscall::PutString(1, (uint64_t)msg1, strlen(msg1), 1, 1, 1);
  syscall::PutString(1, (uint64_t)s, length, 1, 1, 1);
  return syscall::Exit(-1, 1, 1, 1, 1, 1);
}

using SyscallFuncType = syscall::Result (uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t);

extern "C" constexpr unsigned int numSyscall = 0x11;
extern "C" std::array<SyscallFuncType*, numSyscall> syscall_table{
  /* 0x00 */ syscall::LogString,
  /* 0x01 */ syscall::PutString,
  /* 0x02 */ syscall::Exit,
  /* 0x03 */ syscall::OpenWindow,
  /* 0x04 */ syscall::WinWriteString,
  /* 0x05 */ syscall::WinFillRectangle,
  /* 0x06 */ syscall::GetCurrentTick,
  /* 0x07 */ syscall::WinRedraw,
  /* 0x08 */ syscall::WinDrawLine,
  /* 0x09 */ syscall::CloseWindow,
  /* 0x0a */ syscall::ReadEvent,
  /* 0x0b */ syscall::CreateTimer,
  /* 0x0c */ syscall::OpenFile,
  /* 0x0d */ syscall::ReadFile,
  /* 0x0e */ syscall::DemandPages,
  /* 0x0f */ syscall::MapFile,
  /* 0x10 */ syscall::IsTerminal,
};

extern "C" constexpr unsigned int numLinSyscall = 0x9f;
extern "C" std::array<SyscallFuncType*, numLinSyscall> syscall_table_lin{
  /* 0x000 */ syscall::read,
  /* 0x001 */ syscall::write,
  /* 0x002 */ syscall::open,
  /* 0x003 */ syscall::dummy,
  /* 0x004 */ syscall::dummy,
  /* 0x005 */ syscall::dummy,
  /* 0x006 */ syscall::dummy,
  /* 0x007 */ syscall::dummy,
  /* 0x008 */ syscall::dummy,
  /* 0x009 */ syscall::dummy,
  /* 0x00a */ syscall::dummy,
  /* 0x00b */ syscall::dummy,
  /* 0x00c */ syscall::dummy,
  /* 0x00d */ syscall::dummy,
  /* 0x00e */ syscall::dummy,
  /* 0x00f */ syscall::dummy,
  /* 0x010 */ syscall::dummy,
  /* 0x011 */ syscall::dummy,
  /* 0x012 */ syscall::dummy,
  /* 0x013 */ syscall::dummy,
  /* 0x014 */ syscall::dummy,
  /* 0x015 */ syscall::dummy,
  /* 0x016 */ syscall::dummy,
  /* 0x017 */ syscall::dummy,
  /* 0x018 */ syscall::dummy,
  /* 0x019 */ syscall::dummy,
  /* 0x01a */ syscall::dummy,
  /* 0x01b */ syscall::dummy,
  /* 0x01c */ syscall::dummy,
  /* 0x01d */ syscall::dummy,
  /* 0x01e */ syscall::dummy,
  /* 0x01f */ syscall::dummy,
  /* 0x020 */ syscall::dummy,
  /* 0x021 */ syscall::dummy,
  /* 0x022 */ syscall::dummy,
  /* 0x023 */ syscall::dummy,
  /* 0x024 */ syscall::dummy,
  /* 0x025 */ syscall::dummy,
  /* 0x026 */ syscall::dummy,
  /* 0x027 */ syscall::dummy,
  /* 0x028 */ syscall::dummy,
  /* 0x029 */ syscall::dummy,
  /* 0x02a */ syscall::dummy,
  /* 0x02b */ syscall::dummy,
  /* 0x02c */ syscall::dummy,
  /* 0x02d */ syscall::dummy,
  /* 0x02e */ syscall::dummy,
  /* 0x02f */ syscall::dummy,
  /* 0x030 */ syscall::dummy,
  /* 0x031 */ syscall::dummy,
  /* 0x032 */ syscall::dummy,
  /* 0x033 */ syscall::dummy,
  /* 0x034 */ syscall::dummy,
  /* 0x035 */ syscall::dummy,
  /* 0x036 */ syscall::dummy,
  /* 0x037 */ syscall::dummy,
  /* 0x038 */ syscall::dummy,
  /* 0x039 */ syscall::dummy,
  /* 0x03a */ syscall::dummy,
  /* 0x03b */ syscall::dummy,
  /* 0x03c */ syscall::dummy,
  /* 0x03d */ syscall::dummy,
  /* 0x03e */ syscall::dummy,
  /* 0x03f */ syscall::dummy,
  /* 0x040 */ syscall::dummy,
  /* 0x041 */ syscall::dummy,
  /* 0x042 */ syscall::dummy,
  /* 0x043 */ syscall::dummy,
  /* 0x044 */ syscall::dummy,
  /* 0x045 */ syscall::dummy,
  /* 0x046 */ syscall::dummy,
  /* 0x047 */ syscall::dummy,
  /* 0x048 */ syscall::dummy,
  /* 0x049 */ syscall::dummy,
  /* 0x04a */ syscall::dummy,
  /* 0x04b */ syscall::dummy,
  /* 0x04c */ syscall::dummy,
  /* 0x04d */ syscall::dummy,
  /* 0x04e */ syscall::dummy,
  /* 0x04f */ syscall::dummy,
  /* 0x050 */ syscall::dummy,
  /* 0x051 */ syscall::dummy,
  /* 0x052 */ syscall::dummy,
  /* 0x053 */ syscall::dummy,
  /* 0x054 */ syscall::dummy,
  /* 0x055 */ syscall::dummy,
  /* 0x056 */ syscall::dummy,
  /* 0x057 */ syscall::dummy,
  /* 0x058 */ syscall::dummy,
  /* 0x059 */ syscall::dummy,
  /* 0x05a */ syscall::dummy,
  /* 0x05b */ syscall::dummy,
  /* 0x05c */ syscall::dummy,
  /* 0x05d */ syscall::dummy,
  /* 0x05e */ syscall::dummy,
  /* 0x05f */ syscall::dummy,
  /* 0x060 */ syscall::dummy,
  /* 0x061 */ syscall::dummy,
  /* 0x062 */ syscall::dummy,
  /* 0x063 */ syscall::dummy,
  /* 0x064 */ syscall::dummy,
  /* 0x065 */ syscall::dummy,
  /* 0x066 */ syscall::getuid,
  /* 0x067 */ syscall::dummy,
  /* 0x068 */ syscall::getgid,
  /* 0x069 */ syscall::dummy,
  /* 0x06a */ syscall::dummy,
  /* 0x06b */ syscall::geteuid,
  /* 0x06c */ syscall::getegid,
  /* 0x06d */ syscall::dummy,
  /* 0x06e */ syscall::dummy,
  /* 0x06f */ syscall::dummy,
  /* 0x070 */ syscall::dummy,
  /* 0x071 */ syscall::dummy,
  /* 0x072 */ syscall::dummy,
  /* 0x073 */ syscall::dummy,
  /* 0x074 */ syscall::dummy,
  /* 0x075 */ syscall::dummy,
  /* 0x076 */ syscall::dummy,
  /* 0x077 */ syscall::dummy,
  /* 0x078 */ syscall::dummy,
  /* 0x079 */ syscall::dummy,
  /* 0x07a */ syscall::dummy,
  /* 0x07b */ syscall::dummy,
  /* 0x07c */ syscall::dummy,
  /* 0x07d */ syscall::dummy,
  /* 0x07e */ syscall::dummy,
  /* 0x07f */ syscall::dummy,
  /* 0x080 */ syscall::dummy,
  /* 0x081 */ syscall::dummy,
  /* 0x082 */ syscall::dummy,
  /* 0x083 */ syscall::dummy,
  /* 0x084 */ syscall::dummy,
  /* 0x085 */ syscall::dummy,
  /* 0x086 */ syscall::dummy,
  /* 0x087 */ syscall::dummy,
  /* 0x088 */ syscall::dummy,
  /* 0x089 */ syscall::dummy,
  /* 0x08a */ syscall::dummy,
  /* 0x08b */ syscall::dummy,
  /* 0x08c */ syscall::dummy,
  /* 0x08d */ syscall::dummy,
  /* 0x08e */ syscall::dummy,
  /* 0x08f */ syscall::dummy,
  /* 0x090 */ syscall::dummy,
  /* 0x091 */ syscall::dummy,
  /* 0x092 */ syscall::dummy,
  /* 0x093 */ syscall::dummy,
  /* 0x094 */ syscall::dummy,
  /* 0x095 */ syscall::dummy,
  /* 0x096 */ syscall::dummy,
  /* 0x097 */ syscall::dummy,
  /* 0x098 */ syscall::dummy,
  /* 0x099 */ syscall::dummy,
  /* 0x09a */ syscall::dummy,
  /* 0x09b */ syscall::dummy,
  /* 0x09c */ syscall::dummy,
  /* 0x09d */ syscall::dummy,
  /* 0x09e */ syscall::dummy,
  // /* 0x09e */ syscall::arch_prctl,
};


void InitializeSyscall() {
  WriteMSR(kIA32_EFER, 0x0501u);
  WriteMSR(kIA32_LSTAR, reinterpret_cast<uint64_t>(SyscallEntry));
  WriteMSR(kIA32_STAR, static_cast<uint64_t>(8) << 32 |
                       static_cast<uint64_t>(16 | 3) << 48);
  WriteMSR(kIA32_FMASK, 0);
}
