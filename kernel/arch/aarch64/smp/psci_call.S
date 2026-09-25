.text
.global psci_call_smc
psci_call_smc:
    smc #0
    ret
.global psci_call_hvc
psci_call_hvc:
    hvc #0
    ret
