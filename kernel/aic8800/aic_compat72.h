/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Trimui Smart Pro S — AIC8800 (radxa BSP, fdrv 6.4.3.0) build compat shim for
 * mainline v6.15+/v7.x. Force-included via KCFLAGS by build-aic8800.sh; maps the
 * API churn the vendor driver predates. Everything else is fixed in
 * aic8800-7.2.patch. All shims are version-guarded so older kernels are untouched.
 */
#ifndef AIC_COMPAT72_H
#define AIC_COMPAT72_H
#include <linux/version.h>
#include <linux/string.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
/* strncpy() removed from kernel string.h in favour of strscpy(). */
#define strncpy(d, s, n) strscpy(d, s, n)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
/* del_timer()/del_timer_sync() removed; use timer_delete()/timer_delete_sync(). */
#undef del_timer
#undef del_timer_sync
#define del_timer(t)		timer_delete(t)
#define del_timer_sync(t)	timer_delete_sync(t)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
/* from_timer() renamed to timer_container_of(). */
#define from_timer(var, cb, field) timer_container_of(var, cb, field)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
/* in_irq() removed; wakeup_source_create/add/remove/destroy() consolidated into
 * wakeup_source_register()/wakeup_source_unregister().
 */
#define in_irq()			in_hardirq()
#define wakeup_source_create(name)	wakeup_source_register(NULL, name)
#define wakeup_source_add(ws)		do {} while (0)
#define wakeup_source_remove(ws)	do {} while (0)
#define wakeup_source_destroy(ws)	wakeup_source_unregister(ws)
#endif

#endif /* AIC_COMPAT72_H */
