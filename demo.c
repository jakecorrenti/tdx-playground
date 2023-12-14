#include <err.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

int main() {
  int kvm;
  const uint8_t code[] = {
      0xba, 0xf8, 0x03, /* mov $0x3f8, %dx */
      0x00, 0xd8,       /* add %bl, %al */
      0x04, '0',        /* add $'0', %al */
      0xee,             /* out %al, (%dx) */
      0xb0, '\n',       /* mov $'\n', %al */
      0xee,             /* out %al, (%dx) */
      0xf4,             /* hlt */
  };

  // get the kvm device file descriptor.
  // need Read/Write access to the device to setup the virtual machine.
  // all opens not explicitly intended for inheritance across exec should
  // use CLOEXEC
  kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);

  // use the KVM_GET_API_VERSION ioctl() to get the KVM API version
  int ret = ioctl(kvm, KVM_GET_API_VERSION, NULL);
  if (ret == -1)
    err(1, "KVM_GET_API_VERSION");
  if (ret != 12)
    errx(1, "KVM_GET_API_VERSION %d, expected 12", ret);

  // check for any extensions you want to use.
  // for extensions that add new ioctl() calls, you can generally just call the
  // ioctl(), which will fail with an error (ENOTTY) if it does not exist
  ret = ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
  if (ret == -1)
    err(1, "KVM_CHECK_EXTENSION");
  if (!ret)
    errx(1, "Required extension KVM_CAP_USER_MEMORY not available");

  // create a virtual machine which represents everything associated with one
  // emulated system, including memory and one or more CPUs. KVM gives us a
  // handle to this VM in the form of a file descriptor
  int vmfd = ioctl(kvm, KVM_CREATE_VM, (unsigned long)0);

  // VM needs some memory, which we will provide in pages. this corresponds to
  // the "physical" address space as seen by the VM. When a vCPU tries to access
  // memory, the hardware virtualization for that CPU will first try to satisfy
  // that access via the memory pages we've configured. If that fails, the
  // kernel will then let the user of the KVM API handle the acess

  // allocate a single page of memory to hold our code
  uint8_t *mem = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  // copy our machine code into the memory
  memcpy(mem, code, sizeof(code));
  // tell the KVM VM about its new memory
  struct kvm_userspace_memory_region region = {
      .slot = 0, // integer index identifying which memory region we hand to KVM
      .guest_phys_addr = 0x1000, // base "physical" address as seen from the guest
      .memory_size = 0x1000, // how much memory to map: 1 page
      .userspace_addr = (uint64_t)mem, // points to the backing memory in our process that we allocated with mmap()
  };
  ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);

  // KVM vCPU represents the state of one emulated CPU, including processor 
  // registers and other exec state.
  //
  // create vCPU to run the code in memory.
  //
  // (unsigned long)0 represents a sequential vCPU index
  int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long)0);

  // each vCPU has an associated `struct kvm_run` data structure, used to communicate
  // information about the CPU between the kernel and user space. We map this structure
  // into user space using mmap, but first wee need to know how much memory to map
  size_t mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);
  
  // Note: mmap size typically exceeds that of the `kvm_run` structure, as the kernel
  // will use that space to store other transient structures that `kvm_run` might point to
  struct kvm_run *run;
  run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);

  // set up the initial states of special registers
  struct kvm_sregs sregs;
  ioctl(vcpufd, KVM_GET_SREGS, &sregs);
  sregs.cs.base = 0;
  sregs.cs.selector = 0;
  ioctl(vcpufd, KVM_SET_SREGS, &sregs);

  // set up the initial states of standard registers
  struct kvm_regs regs = {
      .rip = 0x1000,
      .rax = 2,
      .rbx = 2,
      .rflags = 0x2,
  };
  ioctl(vcpufd, KVM_SET_REGS, &regs);

  // determine the size of the capabilities struct
  int cpuid_configs_size = 6 * sizeof(struct kvm_tdx_cpuid_config);
  int caps_size = sizeof(struct kvm_tdx_capabilities) + cpuid_configs_size;

  // setup the capabilities struct
  struct kvm_tdx_capabilities *caps;
  caps = (struct kvm_tdx_capabilities *)malloc(caps_size);
  // set # of cpuid configs (https://lkml.kernel.org/kvm/20220802074750.2581308-7-xiaoyao.li@intel.com/)
  // I don't think that this is the right number, though. trying to find this number within the ABI spec.
  // might have to make another ioctl call to get this.
  caps->nr_cpuid_configs = 6;

  struct kvm_tdx_cmd cmd = {
    .id = KVM_TDX_CAPABILITIES,
    .flags = 0,
    .data = (__u64)(unsigned long)caps,
  };
  // cmd.id = KVM_TDX_CAPABILITIES;
  // cmd.flags = 0;
  // cmd.data = (__u64)(unsigned long)caps;

  ret = ioctl(vmfd, KVM_MEMORY_ENCRYPT_OP, &cmd);
  if (ret < 0)
      perror("KVM_TDX_CAPABILITIES error");


  // now that the VM and vCPU are created, memory is mapped and initialized, and initial
  // register states are set, we can$}art running instructions with the vcpu
  while (1) {
      // Note that KVM_RUN runs the VM in the context of the current thread and doesn't
      // return until emulation stops.
      ioctl(vcpufd, KVM_RUN, NULL);
      switch (run->exit_reason) {
        case KVM_EXIT_HLT: 
            puts("KVM_EXIT_HLT");
            return 0;
        case KVM_EXIT_IO:
            if (run->io.direction == KVM_EXIT_IO_OUT &&
                    run->io.size == 1 &&
                    run->io.port == 0x3f8 &&
                    run->io.count == 1)
                putchar(*(((char *)run) + run->io.data_offset));
            else
                errx(1, "unhandled KVM_EXIT_IO");
            break;
        case KVM_EXIT_FAIL_ENTRY:
            errx(1, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx",
                 (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
        case KVM_EXIT_INTERNAL_ERROR:
            errx(1, "KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x",
                 run->internal.suberror);
      }
  }

  return 1;
}
