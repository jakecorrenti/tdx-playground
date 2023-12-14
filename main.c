#include <err.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

int main() {
  int kvm, ret;
  //FIXME: want to be calling on vm fd instead of kvm fd
  // kvm_ioctl_internal

  // get the kvm device file descriptor.
  // need Read/Write access to the device to setup the virtual machine.
  // all opens not explicitly intended for inheritance across exec should
  // use CLOEXEC
  kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);

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

  ret = ioctl(kvm, KVM_TDX_CAPABILITIES, caps);
  
  return 1;
}
