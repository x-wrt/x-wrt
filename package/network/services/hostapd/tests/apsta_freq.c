/* Native test binding for the production frequency helper. */
#include "utils/includes.h"
#include "utils/ucode.h"
#include <ucode/module.h>

__attribute__((visibility("default")))
void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_register(scope, "freq_info", uc_wpa_freq_info);
}
