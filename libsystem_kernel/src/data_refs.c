#include <stdint.h>

/* Data exports from Apple's libsystem_kernel stay in the dyld shared cache.
 * The loader resolves their cache addresses into these pointer variables
 * before any code runs (named _iosw_ref_<name_without_first_underscore>).
 */

__attribute__((visibility("default"))) void *iosw_ref____kernelVersionString = 0;
__attribute__((visibility("default"))) void *iosw_ref____kernelVersionNumber = 0;
__attribute__((visibility("default"))) void *iosw_ref__mach_errors = 0;
__attribute__((visibility("default"))) uint32_t *iosw_ref_port_obj_table_size = 0;
__attribute__((visibility("default"))) void *iosw_ref_important_boost_assertion_token = 0;
__attribute__((visibility("default"))) void *iosw_ref_normal_boost_assertion_token = 0;
__attribute__((visibility("default"))) void *iosw_ref_non_boost_assertion_token = 0;
__attribute__((visibility("default"))) void *iosw_ref_denap_boost_assertion_token = 0;
__attribute__((visibility("default"))) void *iosw_ref_NDR_record = 0;
__attribute__((visibility("default"))) void *iosw_ref_update_tpidr = 0;
__attribute__((visibility("default"))) void *iosw_ref_vprintf_stderr_func = 0;
__attribute__((visibility("default"))) void *iosw_ref_mach_error_full_diag = 0;
__attribute__((visibility("default"))) void *iosw_ref_port_obj_table = 0;
__attribute__((visibility("default"))) uint32_t *iosw_ref__cpu_capabilities = 0;
__attribute__((visibility("default"))) uint32_t *iosw_ref__cpu_has_altivec = 0;
__attribute__((visibility("default"))) void *iosw_ref___syscall_logger = 0;
__attribute__((visibility("default"))) int32_t *iosw_ref__current_pid = 0;
__attribute__((visibility("default"))) uint64_t *iosw_ref_vm_page_size = 0;
__attribute__((visibility("default"))) uint32_t *iosw_ref_vm_page_shift = 0;
__attribute__((visibility("default"))) uint64_t *iosw_ref_vm_page_mask = 0;
__attribute__((visibility("default"))) uint64_t *iosw_ref_vm_kernel_page_size = 0;
__attribute__((visibility("default"))) uint64_t *iosw_ref_vm_kernel_page_mask = 0;
__attribute__((visibility("default"))) uint32_t *iosw_ref_mach_task_self_ = 0;
__attribute__((visibility("default"))) uint32_t *iosw_ref_vm_kernel_page_shift = 0;
__attribute__((visibility("default"))) uint32_t *iosw_ref_bootstrap_port = 0;
__attribute__((visibility("default"))) void *iosw_ref__os_alloc_once_table = 0;
__attribute__((visibility("default"))) int32_t *iosw_ref_errno = 0;
__attribute__((visibility("default"))) void *iosw_ref_internal_catch_exc_subsystem = 0;
