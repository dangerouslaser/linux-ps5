// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/ps5.h>

#include <asm/svm.h>
#include <asm/desc.h>
#include <asm/io.h>
#include <asm/debugreg.h>

void svm_run_guest(u64 a0, u64 a1, u64 a2, u64 a3, u64 vmcb_phys);

static DEFINE_SPINLOCK(svm_execute_lock);

static void svm_set_vmcb_seg(struct vmcb_seg *s, u16 selector, u64 base, u32 limit,
			     struct desc_struct *d)
{
	s->base = base;
	s->limit = limit;
	s->selector = selector;
	s->attrib = 0;

	if (d) {
		if (d->g)
			s->limit = (limit << 12) | 0xfff;

		s->attrib |= (d->type & SVM_SELECTOR_TYPE_MASK);
		s->attrib |= (d->s & 1) << SVM_SELECTOR_S_SHIFT;
		s->attrib |= (d->dpl & 3) << SVM_SELECTOR_DPL_SHIFT;
		s->attrib |= (d->p & 1) << SVM_SELECTOR_P_SHIFT;
		s->attrib |= (d->avl & 1) << SVM_SELECTOR_AVL_SHIFT;
		s->attrib |= (d->l & 1) << SVM_SELECTOR_L_SHIFT;
		s->attrib |= (d->d & 1) << SVM_SELECTOR_DB_SHIFT;
		s->attrib |= (d->g & 1) << SVM_SELECTOR_G_SHIFT;
	}
}

static void svm_set_segment(struct vmcb_seg *seg, u16 selector, struct desc_ptr *desc)
{
	struct desc_struct *d = NULL;
	u64 base = 0;
	u32 limit = 0;

	if (selector) {
		d = &((struct desc_struct *)desc->address)[selector >> 3];
		base = get_desc_base(d);
		limit = get_desc_limit(d);
	}

	svm_set_vmcb_seg(seg, selector, base, limit, d);
}

static void svm_guest_entry(void)
{
	asm volatile(
		ANNOTATE_RETPOLINE_SAFE "\n\t"
		"call *%%rax\n\t"
		"vmmcall\n\t"
		: ASM_CALL_CONSTRAINT
		:
		: "memory"
	);
}

static void svm_init_vmcb(struct vmcb *vmcb, void *guest_func, void *guest_stack)
{
	struct vmcb_save_area *save = &vmcb->save;
	struct vmcb_control_area *ctrl = &vmcb->control;
	u64 vmcb_phys = virt_to_phys(vmcb);
	struct desc_ptr gdt_ptr, idt_ptr;
	u16 sel;
	unsigned long dr6, dr7;

	memset(vmcb, 0, PAGE_SIZE);

	asm volatile("vmsave %0" : : "a"(vmcb_phys) : "memory");

	native_store_gdt(&gdt_ptr);
	store_idt(&idt_ptr);

	savesegment(es, sel); svm_set_segment(&save->es, sel, &gdt_ptr);
	savesegment(cs, sel); svm_set_segment(&save->cs, sel, &gdt_ptr);
	savesegment(ss, sel); svm_set_segment(&save->ss, sel, &gdt_ptr);
	savesegment(ds, sel); svm_set_segment(&save->ds, sel, &gdt_ptr);

	svm_set_vmcb_seg(&save->gdtr, 0, gdt_ptr.address, gdt_ptr.size, NULL);
	svm_set_vmcb_seg(&save->idtr, 0, idt_ptr.address, idt_ptr.size, NULL);

	save->cr0 = read_cr0();
	save->cr2 = read_cr2();
	save->cr3 = __read_cr3();
	save->cr4 = __read_cr4();

	get_debugreg(dr6, 6);
	get_debugreg(dr7, 7);
	save->dr6 = dr6;
	save->dr7 = dr7;

	save->rflags = native_save_fl();

	save->rip = (u64)svm_guest_entry;
	save->rax = (u64)guest_func;
	save->rsp = (u64)guest_stack + PAGE_SIZE;

	rdmsrq(MSR_EFER, save->efer);
	rdmsrq(MSR_IA32_CR_PAT, save->g_pat);

	__set_bit(INTERCEPT_VMRUN, (unsigned long *)ctrl->intercepts);
	__set_bit(INTERCEPT_VMMCALL, (unsigned long *)ctrl->intercepts);

	ctrl->asid = 1;
	ctrl->tlb_ctl = TLB_CONTROL_FLUSH_ASID;
}

u64 svm_execute_guest(void *guest_func, u64 a0, u64 a1, u64 a2, u64 a3)
{
	unsigned long flags;
	u64 efer, hsave_pa;
	u64 exit_code;
	u64 ret = -1;
	struct vmcb *vmcb = NULL;
	void *hsave_area = NULL;
	void *guest_stack = NULL;

	vmcb = (struct vmcb *)get_zeroed_page(GFP_KERNEL);
	if (!vmcb)
		goto err_free;

	hsave_area = (void *)get_zeroed_page(GFP_KERNEL);
	if (!hsave_area)
		goto err_free;

	guest_stack = (void *)get_zeroed_page(GFP_KERNEL);
	if (!guest_stack)
		goto err_free;

	spin_lock_irqsave(&svm_execute_lock, flags);

	rdmsrq(MSR_EFER, efer);
	wrmsrq(MSR_EFER, efer | EFER_SVME);

	rdmsrq(MSR_VM_HSAVE_PA, hsave_pa);
	wrmsrq(MSR_VM_HSAVE_PA, virt_to_phys(hsave_area));

	svm_init_vmcb(vmcb, guest_func, guest_stack);

	svm_run_guest(a0, a1, a2, a3, virt_to_phys(vmcb));

	exit_code = vmcb->control.exit_code;
	if (exit_code == SVM_EXIT_VMMCALL) {
		ret = vmcb->save.rax;
	} else {
		pr_warn("svm: unexpected exit code 0x%llx\n", exit_code);
	}

	wrmsrq(MSR_VM_HSAVE_PA, hsave_pa);
	wrmsrq(MSR_EFER, efer);

	spin_unlock_irqrestore(&svm_execute_lock, flags);

err_free:
	if (guest_stack)
		free_page((unsigned long)guest_stack);
	if (hsave_area)
		free_page((unsigned long)hsave_area);
	if (vmcb)
		free_page((unsigned long)vmcb);

	return ret;
}
EXPORT_SYMBOL(svm_execute_guest);
