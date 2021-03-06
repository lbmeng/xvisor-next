/**
 * Copyright (c) 2018 Anup Patel.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file cpu_vcpu_helper.c
 * @author Anup Patel (anup@brainfault.org)
 * @brief source of VCPU helper functions
 */

#include <vmm_devtree.h>
#include <vmm_error.h>
#include <vmm_heap.h>
#include <vmm_smp.h>
#include <vmm_stdio.h>
#include <vmm_pagepool.h>
#include <vmm_timer.h>
#include <vmm_host_aspace.h>
#include <arch_barrier.h>
#include <arch_guest.h>
#include <arch_vcpu.h>
#include <vio/vmm_vserial.h>
#include <libs/mathlib.h>

#include <cpu_hwcap.h>
#include <cpu_tlb.h>
#include <cpu_mmu.h>
#include <cpu_vcpu_fp.h>
#include <cpu_vcpu_helper.h>
#include <cpu_vcpu_timer.h>
#include <cpu_guest_serial.h>
#include <riscv_csr.h>

static char *guest_fdt_find_serial_node(char *guest_name)
{
	char *serial = NULL;
	char chosen_node_path[VMM_FIELD_NAME_SIZE];
	struct vmm_devtree_node *chosen_node;

	/* Process attributes in chosen node */
	vmm_snprintf(chosen_node_path, sizeof(chosen_node_path), "/%s/%s/%s",
		     VMM_DEVTREE_GUESTINFO_NODE_NAME, guest_name,
		     VMM_DEVTREE_CHOSEN_NODE_NAME);
	chosen_node = vmm_devtree_getnode(chosen_node_path);
	if (chosen_node) {
		/* Process console device passed via chosen node */
		vmm_devtree_read_string(chosen_node,
					VMM_DEVTREE_STDOUT_ATTR_NAME,
					(const char **)&serial);
	}
	vmm_devtree_dref_node(chosen_node);

	return serial;
}

static int guest_vserial_notification(struct vmm_notifier_block *nb,
					unsigned long evt, void *data)
{
	int ret = NOTIFY_OK;
	struct vmm_vserial_event *e = data;
	struct riscv_guest_serial *gserial = container_of(nb,
						struct riscv_guest_serial,
						vser_client);
	if (evt ==  VMM_VSERIAL_EVENT_CREATE) {
		if (!strcmp(e->vser->name, gserial->name)) {
			gserial->vserial = e->vser;
		}
	} else if (evt == VMM_VSERIAL_EVENT_DESTROY) {
		if (!strcmp(e->vser->name, gserial->name))
			gserial->vserial = NULL;
	} else
		ret = NOTIFY_DONE;
	return ret;
}

int arch_guest_init(struct vmm_guest *guest)
{
	struct riscv_guest_priv *priv;
	struct riscv_guest_serial *gserial;
	char *sname;
	int rc;

	if (!guest->reset_count) {
		if (!riscv_hyp_ext_enabled)
			return VMM_EINVALID;

		guest->arch_priv = vmm_malloc(sizeof(struct riscv_guest_priv));
		if (!guest->arch_priv) {
			return VMM_ENOMEM;
		}
		priv = riscv_guest_priv(guest);

		priv->time_offset = vmm_manager_guest_reset_timestamp(guest);
		priv->time_offset =
			priv->time_offset * vmm_timer_clocksource_frequency();
		priv->time_offset = udiv64(priv->time_offset, 1000000000ULL);

		priv->pgtbl = cpu_mmu_pgtbl_alloc(PGTBL_STAGE2);
		if (!priv->pgtbl) {
			vmm_free(guest->arch_priv);
			guest->arch_priv = NULL;
			return VMM_ENOMEM;
		}
		priv->guest_serial = vmm_malloc(sizeof(struct riscv_guest_serial));
		if (!priv->guest_serial)
			return VMM_ENOMEM;

		gserial = riscv_guest_serial(guest);
		sname = guest_fdt_find_serial_node(guest->name);
		if (sname) {
			strlcpy(gserial->name, guest->name,
				sizeof(gserial->name));
			strlcat(gserial->name, "/", sizeof(gserial->name));
			strlcat(gserial->name, sname, sizeof(gserial->name));
		}
		gserial->vser_client.notifier_call = &guest_vserial_notification;
		gserial->vser_client.priority = 0;
		rc = vmm_vserial_register_client(&gserial->vser_client);
		if (rc)
			return rc;
	}

	return VMM_OK;
}

int arch_guest_deinit(struct vmm_guest *guest)
{
	int rc;
	struct riscv_guest_serial *gs;

	if (guest->arch_priv) {
		gs = riscv_guest_serial(guest);

		if ((rc = cpu_mmu_pgtbl_free(riscv_guest_priv(guest)->pgtbl))) {
			return rc;
		}
		if (gs) {
			vmm_vserial_unregister_client(&gs->vser_client);
			vmm_free(gs);
		}
		vmm_free(guest->arch_priv);
	}

	return VMM_OK;
}

int arch_guest_add_region(struct vmm_guest *guest, struct vmm_region *region)
{
	return VMM_OK;
}

int arch_guest_del_region(struct vmm_guest *guest, struct vmm_region *region)
{
	return VMM_OK;
}

int arch_vcpu_init(struct vmm_vcpu *vcpu)
{
	int rc = VMM_OK;
	const char *attr;
	virtual_addr_t sp, sp_exec;

	/* Determine stack location */
	if (vcpu->is_normal) {
		sp = 0;
		sp_exec = vcpu->stack_va + vcpu->stack_sz;
		sp_exec = sp_exec & ~(__SIZEOF_POINTER__ - 1);
	} else {
		sp = vcpu->stack_va + vcpu->stack_sz;
		sp = sp & ~(__SIZEOF_POINTER__ - 1);
		if (!vcpu->reset_count) {
			/* First time allocate exception stack */
			sp_exec = vmm_pagepool_alloc(VMM_PAGEPOOL_NORMAL,
				VMM_SIZE_TO_PAGE(CONFIG_IRQ_STACK_SIZE));
			if (!sp_exec) {
				return VMM_ENOMEM;
			}
			sp_exec += CONFIG_IRQ_STACK_SIZE;
		} else {
			sp_exec = riscv_regs(vcpu)->sp_exec;
		}
	}

	/* For both Orphan & Normal VCPUs */
	memset(riscv_regs(vcpu), 0, sizeof(arch_regs_t));
	riscv_regs(vcpu)->sepc = vcpu->start_pc;
	riscv_regs(vcpu)->sstatus = SSTATUS_SPP | SSTATUS_SPIE;
	riscv_regs(vcpu)->sp = sp;
	riscv_regs(vcpu)->sp_exec = sp_exec;
	riscv_regs(vcpu)->hstatus = 0;

	/* For Orphan VCPUs we are done */
	if (!vcpu->is_normal) {
		return VMM_OK;
	}

	/* Following initialization for normal VCPUs only */
	rc = vmm_devtree_read_string(vcpu->node,
			VMM_DEVTREE_COMPATIBLE_ATTR_NAME, &attr);
	if (rc) {
		goto done;
	}
#if __riscv_xlen == 64
	if (strcmp(attr, "riscv64,generic") != 0) {
#elif __riscv_xlen == 32
	if (strcmp(attr, "riscv32,generic") != 0) {
#else
#error "Unexpected __riscv_xlen"
#endif
		rc = VMM_EINVALID;
		goto done;
	}

	/* Set a0 to VCPU sub-id (i.e. virtual HARTID) */
	riscv_regs(vcpu)->a0 = vcpu->subid;

	/* Update HSTATUS */
	riscv_regs(vcpu)->hstatus |= HSTATUS_SP2V;
	riscv_regs(vcpu)->hstatus |= HSTATUS_SP2P;
	riscv_regs(vcpu)->hstatus |= HSTATUS_SPV;

	/* First time initialization of private context */
	if (!vcpu->reset_count) {
		/* Alloc private context */
		vcpu->arch_priv = vmm_zalloc(sizeof(struct riscv_priv));
		if (!vcpu->arch_priv) {
			rc = VMM_ENOMEM;
			goto done;
		}
	}

	/* Update BS<xyz> */
	riscv_priv(vcpu)->bsstatus = 0; /* TODO: ??? */
	riscv_priv(vcpu)->bsie = 0;
	riscv_priv(vcpu)->bstvec = 0;
	riscv_priv(vcpu)->bsscratch = 0;
	riscv_priv(vcpu)->bsepc = 0;
	riscv_priv(vcpu)->bscause = 0;
	riscv_priv(vcpu)->bstval = 0;
	riscv_priv(vcpu)->bsip = 0;
	riscv_priv(vcpu)->bsatp = 0;

	/* Update HIDELEG */
	riscv_priv(vcpu)->hideleg = 0;
	riscv_priv(vcpu)->hideleg |= SIP_SSIP;
	riscv_priv(vcpu)->hideleg |= SIP_STIP;
	riscv_priv(vcpu)->hideleg |= SIP_SEIP;

	/* Update HEDELEG */
	riscv_priv(vcpu)->hedeleg = 0;
	riscv_priv(vcpu)->hedeleg |= (1U << CAUSE_MISALIGNED_FETCH);
	riscv_priv(vcpu)->hedeleg |= (1U << CAUSE_BREAKPOINT);
	riscv_priv(vcpu)->hedeleg |= (1U << CAUSE_USER_ECALL);
	riscv_priv(vcpu)->hedeleg |= (1U << CAUSE_FETCH_PAGE_FAULT);
	riscv_priv(vcpu)->hedeleg |= (1U << CAUSE_LOAD_PAGE_FAULT);
	riscv_priv(vcpu)->hedeleg |= (1U << CAUSE_STORE_PAGE_FAULT);

	/* Initialize FP state */
	cpu_vcpu_fp_init(vcpu);

	riscv_timer_event_init(vcpu, &riscv_timer_priv(vcpu));
done:
	return rc;
}

int arch_vcpu_deinit(struct vmm_vcpu *vcpu)
{
	int rc;
	virtual_addr_t sp_exec;

	/* Free-up exception stack for Orphan VCPU */
	if (!vcpu->is_normal) {
		sp_exec = riscv_regs(vcpu)->sp_exec - CONFIG_IRQ_STACK_SIZE;
		vmm_pagepool_free(VMM_PAGEPOOL_NORMAL, sp_exec,
				  VMM_SIZE_TO_PAGE(CONFIG_IRQ_STACK_SIZE));
	}

	/* For both Orphan & Normal VCPUs */

	/* Clear arch registers */
	memset(riscv_regs(vcpu), 0, sizeof(arch_regs_t));

	/* For Orphan VCPUs do nothing else */
	if (!vcpu->is_normal) {
		return VMM_OK;
	}

	rc = riscv_timer_event_deinit(vcpu, &riscv_timer_priv(vcpu));
	if (rc)
		return rc;

	/* Free private context */
	vmm_free(vcpu->arch_priv);
	vcpu->arch_priv = NULL;

	return VMM_OK;
}

void arch_vcpu_switch(struct vmm_vcpu *tvcpu,
		      struct vmm_vcpu *vcpu,
		      arch_regs_t *regs)
{
	struct riscv_priv *priv;

	if (tvcpu) {
		memcpy(riscv_regs(tvcpu), regs, sizeof(*regs));
		if (tvcpu->is_normal) {
			priv = riscv_priv(tvcpu);
			priv->hideleg = csr_read(CSR_HIDELEG);
			priv->hedeleg = csr_read(CSR_HEDELEG);
			priv->bsstatus = csr_read(CSR_BSSTATUS);
			priv->bsie = csr_read(CSR_BSIE);
			priv->bstvec = csr_read(CSR_BSTVEC);
			priv->bsscratch = csr_read(CSR_BSSCRATCH);
			priv->bsepc = csr_read(CSR_BSEPC);
			priv->bscause = csr_read(CSR_BSCAUSE);
			priv->bstval = csr_read(CSR_BSTVAL);
			priv->bsip = csr_read(CSR_BSIP);
			priv->bsatp = csr_read(CSR_BSATP);
			cpu_vcpu_fp_save(tvcpu, regs);
		}
	}

	memcpy(regs, riscv_regs(vcpu), sizeof(*regs));
	if (vcpu->is_normal) {
		priv = riscv_priv(vcpu);
		csr_write(CSR_HIDELEG, priv->hideleg);
		csr_write(CSR_HEDELEG, priv->hedeleg);
		csr_write(CSR_BSSTATUS, priv->bsstatus);
		csr_write(CSR_BSIE, priv->bsie);
		csr_write(CSR_BSTVEC, priv->bstvec);
		csr_write(CSR_BSSCRATCH, priv->bsscratch);
		csr_write(CSR_BSEPC, priv->bsepc);
		csr_write(CSR_BSCAUSE, priv->bscause);
		csr_write(CSR_BSTVAL, priv->bstval);
		csr_write(CSR_BSIP, priv->bsip);
		csr_write(CSR_BSATP, priv->bsatp);
		cpu_vcpu_fp_restore(vcpu, regs);
		if (CONFIG_MAX_GUEST_COUNT <= (1UL << riscv_vmid_bits)) {
			cpu_mmu_stage2_change_pgtbl(vcpu->guest->id,
					riscv_guest_priv(vcpu->guest)->pgtbl);
		} else {
			cpu_mmu_stage2_change_pgtbl(0,
					riscv_guest_priv(vcpu->guest)->pgtbl);
			__hfence_gvma_all();
		}
	}
}

void arch_vcpu_post_switch(struct vmm_vcpu *vcpu,
			   arch_regs_t *regs)
{
	/* Nothing to do here */
}

void cpu_vcpu_dump_general_regs(struct vmm_chardev *cdev,
				arch_regs_t *regs)
{
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "     zero", regs->zero, "       ra", regs->ra);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       sp", regs->sp, "       gp", regs->gp);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       tp", regs->tp, "       s0", regs->s0);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       s1", regs->s1, "       a0", regs->a0);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       a1", regs->a1, "       a2", regs->a2);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       a3", regs->a3, "       a4", regs->a4);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       a5", regs->a5, "       a6", regs->a6);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       a7", regs->a7, "       s2", regs->s2);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       s3", regs->s3, "       s4", regs->s4);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       s5", regs->s5, "       s6", regs->s6);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       s7", regs->s7, "       s8", regs->s8);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       s9", regs->s9, "      s10", regs->s10);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "      s11", regs->s11, "       t0", regs->t0);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       t1", regs->t1, "       t2", regs->t2);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       t3", regs->t3, "       t4", regs->t4);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "       t5", regs->t5, "       t6", regs->t6);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "     sepc", regs->sepc, "  sstatus", regs->sstatus);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "  hstatus", regs->hstatus, "  sp_exec", regs->sp_exec);
}

void cpu_vcpu_dump_private_regs(struct vmm_chardev *cdev,
				struct vmm_vcpu *vcpu)
{
	struct riscv_priv *priv = riscv_priv(vcpu);

	vmm_cprintf(cdev, "\n");
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "  hedeleg", priv->hedeleg, "  hideleg", priv->hideleg);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    " bsstatus", priv->bsstatus, "     bsie", priv->bsie);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "   bstvec", priv->bstvec, "bsscratch", priv->bsscratch);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "    bsepc", priv->bsepc, "  bscause", priv->bscause);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "   bstval", priv->bstval, "     bsip", priv->bsip);
	vmm_cprintf(cdev, "%s=0x%"PRIADDR"\n",
		    "    bsatp", priv->bsatp);

	cpu_vcpu_fp_dump_regs(cdev, vcpu);
}

void cpu_vcpu_dump_exception_regs(struct vmm_chardev *cdev,
				  unsigned long scause,
				  unsigned long stval)
{
	vmm_cprintf(cdev, "%s=0x%"PRIADDR" %s=0x%"PRIADDR"\n",
		    "   scause", scause, "    stval", stval);
}

void arch_vcpu_regs_dump(struct vmm_chardev *cdev, struct vmm_vcpu *vcpu)
{
	cpu_vcpu_dump_general_regs(cdev, riscv_regs(vcpu));

	if (vcpu->is_normal) {
		cpu_vcpu_dump_private_regs(cdev, vcpu);
	}
}

void arch_vcpu_stat_dump(struct vmm_chardev *cdev, struct vmm_vcpu *vcpu)
{
	/* For now no arch specific stats */
}
