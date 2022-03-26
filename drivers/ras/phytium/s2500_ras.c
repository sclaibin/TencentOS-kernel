/*
 * phytiumlicon ras driver
 *
 * Copyright (c) 2016-2022 phytiumlicon Limited.
 *
 * YinFeng Wang <wangyinfeng@phytium.com>

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/device.h>
#include <linux/phytium/ras/s2500_ras.h>

#define ENABLE_DLU                          1
#define ENABLE_DCU                          1
#define ENABLE_MCU                          1
#define ENABLE_PEU0                         1
#define ENABLE_LIU                          1

#define SOCKET_NUM                          2
#define MAX_PANEL                           8

#define SKT_BASE(skt)                       (void __iomem *)(long unsigned int)((long unsigned int)(skt) << 41)

/* caculate dlu dfx base address */
#define DLU_DFX_BASE						0x28804000
#define SKT_DLU_DFX_BASE(skt, fit)        (void __iomem *)((long unsigned int)SKT_BASE(skt) | \
                                                             (long unsigned int)(DLU_DFX_BASE) | \
                                                             (long unsigned int)((fit) << 16))

/* caculate dcu dfx base address */
#define DCU_DFX_BASE                        0x28702000
#define DCU_DFX_ODD_OFFSET                      0x8000
#define SKT_DCU_DFX_BASE(skt, dcuid)       (void __iomem *)((long unsigned int)SKT_BASE(skt) | \
                                                             (long unsigned int) DCU_DFX_BASE | \
                                                             (long unsigned int) ((dcuid / 2) << 16) | \
                                                             (long unsigned int)(DCU_DFX_ODD_OFFSET*(dcuid % 2)))

/* caculate mcu dfx base address */
#define MCU_DFX_BASE                        0x28780000
#define SKT_MCUX_DFX_BASE(skt, lmuid)        (void __iomem *)((long unsigned int)SKT_BASE(skt) | \
                                                             (long unsigned int)(MCU_DFX_BASE) | \
                                                             (long unsigned int)((lmuid) << 16))

#define SKT_PEU0_DFX_BASE(skt)              (void __iomem *) ((long unsigned int)SKT_BASE(skt) | (long unsigned int)0x2CB00000)

#define LIU_DFX_BASE                    	0x28842000
#define SKT_LIU_DFX_BASE(skt)				(void __iomem *)((long unsigned int)SKT_BASE(skt) | \
                                                             (long unsigned int)LIU_DFX_BASE)

/* ras ERR_STATUS register bitmaps */
#define RAS_ERR_STATUS(n)                   (0x010 + (n) * 64)
#define RAS_ERR_MISC0(n)                    (0x24 + (n) * 64)
#define RAS_ERR_STATUS_V                    (1<<30)
#define RAS_ERR_STATUS_UE                   (1<<29)
#define RAS_ERR_STATUS_DE                   (1<<23)
#define RAS_ERR_STATUS_CE                   (0x3<<24)
#define RAS_ERR_STATUS_OF                   (0x1<<27)

#define RAS_ERR_IDR                         (0xFC8)             /* record Error id register */
#define RAS_ERR_GSR                         (0xE00)             /* Error group register */

static void *dfx_dlu_base[SOCKET_NUM][4];
static void *dfx_dcu_base[SOCKET_NUM][16];
static void *dfx_mcu_base[SOCKET_NUM][MAX_PANEL];
static void *dfx_peu0_base[SOCKET_NUM];
static void *dfx_liu_base[SOCKET_NUM];

/*
 * Print DLU DFX registers
 */
#ifdef ENABLE_DLU
static void phytium_print_dlu_dfx_info(void)
{
    int i, j;
    for (i = 0 ; i < SOCKET_NUM; i++)
        for (j=0; j<4; j++)
            printk("SKT(%d), FIT(%d)_RETRANS_REG: 0x%x\n", i, j, readl(dfx_dlu_base[i][j] + FITx_REG));

    return ;
}
#endif

/*
 * Print DCU DFX registers
 */
#ifdef ENABLE_DCU
static void phytium_print_dcu_dfx_info(void)
{
    int i, j;
    for (i = 0 ; i < SOCKET_NUM; i++)
        for (j=0; j<16; j++) {
            printk("SKT(%d), DCU(%d)_ERR_STATUS_REG: 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_ERR_STATUS_REG));
            printk("SKT(%d), DCU(%d)_ERR_LOGEN_REG : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_ERR_LOGEN_REG));
            printk("SKT(%d), DCU(%d)_ERR_EN_REG    : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_ERR_EN_REG));
            printk("SKT(%d), DCU(%d)_INT_EN_REG    : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_INT_EN_REG));
            printk("SKT(%d), DCU(%d)_ERR_SET_REG   : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_ERR_SET_REG));
            printk("SKT(%d), DCU(%d)_ERR_CLR_REG   : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_ERR_CLR_REG));
            printk("SKT(%d), DCU(%d)_DIR_ERR0_REG  : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_DIR_ERR0_REG));
            printk("SKT(%d), DCU(%d)_DIR_ERR1_REG  : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_DIR_ERR1_REG));
            printk("SKT(%d), DCU(%d)_DIR_ERR2_REG  : 0x%x\n", i, j, readl(dfx_dcu_base[i][j] + DCU_DIR_ERR2_REG));
        }

    return ;
}
#endif

/*
 * Print MCU DFX registers
 */
#ifdef ENABLE_MCU
static void phytium_print_mcu_dfx_info(void)
{
    int i, j;
    for (i = 0 ; i < SOCKET_NUM; i++)
        for (j=0; j < MAX_PANEL; j++) {
            writel(MCU_ERR_STA_REG, dfx_mcu_base[i][j]);
            printk("SKT(%d), MCU(%d)_ERR_STA_REG: 0x%x\n", i, j, readl(dfx_mcu_base[i][j] + 0x8));
            writel(MCU_LOG_ENA_REG, dfx_mcu_base[i][j]);
            printk("SKT(%d), MCU(%d)_LOG_ENA_REG: 0x%x\n", i, j, readl(dfx_mcu_base[i][j] + 0x8));
            writel(MCU_ERR_ENA_REG, dfx_mcu_base[i][j]);
            printk("SKT(%d), MCU(%d)_ERR_ENA_REG: 0x%x\n", i, j, readl(dfx_mcu_base[i][j] + 0x8));
            writel(MCU_INT_ENA_REG, dfx_mcu_base[i][j]);
            printk("SKT(%d), MCU(%d)_INT_ENA_REG: 0x%x\n", i, j, readl(dfx_mcu_base[i][j] + 0x8));
            writel(MCU_ERR_SET_REG, dfx_mcu_base[i][j]);
            printk("SKT(%d), MCU(%d)_ERR_SET_REG: 0x%x\n", i, j, readl(dfx_mcu_base[i][j] + 0x8));
            writel(MCU_ERR_SET_REG, dfx_mcu_base[i][j]);
            printk("SKT(%d), MCU(%d)_ERR_CLR_REG: 0x%x\n", i, j, readl(dfx_mcu_base[i][j] + 0x8));
        }

    return ;
}
#endif

/*
 * Print PEU0 DFX registers
 */
#ifdef ENABLE_PEU0
static void phytium_print_peu_dfx_info(void)
{
    int i;
    for (i = 0 ; i < SOCKET_NUM; i++)
        printk("SKT(%d), PEU_ERR_STA_REG: 0x%x\n", i, readl(dfx_peu0_base[i] + PEU_ERR_STA_REG));

    return ;
}
#endif

/*
 * Print LIU DFX registers
 */
#ifdef ENABLE_LIU
static void phytium_print_liu_dfx_info(void)
{
    int i;
    for (i = 0 ; i < SOCKET_NUM; i++) {
        printk("SKT(%d), LIU_ERR0_FR    : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERR0_FR));
        printk("SKT(%d), LIU_ERR0_CTLR  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERR0_CTLR));
        printk("SKT(%d), LIU_ERR0_STATUS: 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERR0_STATUS));
        printk("SKT(%d), LIU_ERR0_ADDR  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERR0_ADDR));
        printk("SKT(%d), LIU_ERR0_MISC0 : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERR0_MISC0));
        printk("SKT(%d), LIU_ERR0_MISC1 : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERR0_MISC1));
        printk("SKT(%d), LIU_ERRGSR0    : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRGSR0));
        printk("SKT(%d), LIU_ERRFHICR0  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRFHICR0));
        printk("SKT(%d), LIU_ERRFHICR1  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRFHICR1));
        printk("SKT(%d), LIU_ERRFHICR2  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRFHICR2));
        printk("SKT(%d), LIU_ERRERICR0  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRERICR0));
        printk("SKT(%d), LIU_ERRERICR1  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRERICR1));
        printk("SKT(%d), LIU_ERRERICR2  : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRERICR2));
        printk("SKT(%d), LIU_ERRIRQSR   : 0x%x\n", i, readl(dfx_liu_base[i] + LIU_ERRIRQSR));
    }

    return ;
}
#endif

void s2500_print_all_dfx_info(void)
{
#ifdef ENABLE_DLU
    phytium_print_dlu_dfx_info();
#endif

#ifdef ENABLE_DCU
    phytium_print_dcu_dfx_info();
#endif

#ifdef ENABLE_MCU
    phytium_print_mcu_dfx_info();
#endif

#ifdef ENABLE_PEU0
    phytium_print_peu_dfx_info();
#endif

#ifdef ENABLE_LIU
    phytium_print_liu_dfx_info();
#endif

    return ;
}
EXPORT_SYMBOL_GPL(s2500_print_all_dfx_info);

static int phytium_dfx_init(void)
{

    int i, j;
    printk("%s_%d, Start!\n", __func__, __LINE__);
    for (i = 0 ; i < SOCKET_NUM; i++) {
#ifdef ENABLE_DLU
        /* dlu dfx space ioremap */
        for (j = 0; j < 4; j++) {
			dfx_dlu_base[i][j] = ioremap_nocache((unsigned long)SKT_DLU_DFX_BASE(i, j), (unsigned long)0x100);
	        if(IS_ERR(dfx_dlu_base[i][j])) {
	        	pr_info("socket (%d), fit (%d) ioremap error\n", i, j);
	        	goto error;
	        }
        }
#endif

#ifdef ENABLE_DCU
        /* dcu dfx space ioremap */
        for (j = 0; j < 16; j++) {
			dfx_dcu_base[i][j] = ioremap_nocache((unsigned long)SKT_DCU_DFX_BASE(i, j), (unsigned long)0x1000);
	        if(IS_ERR(dfx_dcu_base[i][j])) {
	        	pr_info("socket (%d), dcuid (%d) ioremap error\n", i, j);
	        	goto error;
	        }
        }
#endif

#ifdef ENABLE_MCU
        /* LMU ras space ioremap */
        for (j = 0; j< MAX_PANEL; j++) {
            dfx_mcu_base[i][j] = ioremap_nocache((unsigned long)SKT_MCUX_DFX_BASE(i, j), (unsigned long)0x1000);
	        if(IS_ERR(dfx_mcu_base[i][j])) {
	        	pr_err("socket (%d), mcuid (%d) ioremap error\n", i, j);
	        	goto error;
	        }
        }
#endif

#ifdef ENABLE_PEU0
        /* peu0 ras space ioremap*/
        dfx_peu0_base[i] = ioremap_nocache((unsigned long)SKT_PEU0_DFX_BASE(i), (unsigned long)0x100);
	    if(IS_ERR(dfx_peu0_base[i])) {
	    	pr_err("socket (%d), peu0 ioremap error\n", i);
	    	goto error;
	    }
#endif

#ifdef ENABLE_LIU
        /* liu dfx space ioremap*/
        dfx_liu_base[i] = ioremap_nocache((unsigned long)SKT_LIU_DFX_BASE(i), (unsigned long)0x1000);
	    if(IS_ERR(dfx_liu_base[i])) {
	    	pr_err("socket (%d), LIU ioremap error\n", i);
	    	goto error;
	    }
#endif
    }

    s2500_print_all_dfx_info();
    printk("%s_%d, END!\n", __func__, __LINE__);

	return 0;

error:
    for (i = 0 ; i < SOCKET_NUM; i++) {
#ifdef ENABLE_DLU
        for (j = 0; j< 4; j++) {
	        if(!IS_ERR(dfx_dlu_base[i][j]))
                iounmap(dfx_dlu_base[i][j]);
        }
#endif

#ifdef ENABLE_DCU
        for (j = 0; j< 16; j++) {
	        if(!IS_ERR(dfx_dcu_base[i][j]))
                iounmap(dfx_dcu_base[i][j]);
        }
#endif

#ifdef ENABLE_MCU
        for (j = 0; j< MAX_PANEL; j++)
	        if(IS_ERR(dfx_mcu_base[i][j]))
                iounmap(dfx_mcu_base[i][j]);
#endif

#ifdef ENABLE_PEU0
        if(IS_ERR(dfx_peu0_base[i]))
            iounmap(dfx_peu0_base[i]);
#endif

#ifdef ENABLE_LIU
        if(IS_ERR(dfx_liu_base[i]))
            iounmap(dfx_liu_base[i]);
#endif
    }
	return -EINVAL;
}

static void phytium_dfx_remove(void)
{
    int i, j;

    for (i = 0 ; i < SOCKET_NUM; i++) {
#ifdef ENABLE_DLU
        for (j = 0; j< 4; j++)
            if(!IS_ERR(dfx_dlu_base[i][j]))
                iounmap(dfx_dlu_base[i][j]);
#endif

#ifdef ENABLE_DCU
        for (j = 0; j< 16; j++)
            if(!IS_ERR(dfx_dcu_base[i][j]))
                iounmap(dfx_dcu_base[i][j]);
#endif

#ifdef ENABLE_MCU
        for (j = 0; j< MAX_PANEL; j++)
            if(!IS_ERR(dfx_mcu_base[i][j]))
                iounmap(dfx_mcu_base[i][j]);
#endif

#ifdef ENABLE_PEU0
        if(!IS_ERR(dfx_peu0_base[i]))
            iounmap(dfx_peu0_base[i]);
#endif

#ifdef ENABLE_LIU
        if(!IS_ERR(dfx_liu_base[i]))
            iounmap(dfx_liu_base[i]);
#endif
    }

    return ;
}

static int ras_init(void)
{
	return phytium_dfx_init();
}

static void ras_exit(void)
{
	phytium_dfx_remove();
}

module_init(ras_init);
module_exit(ras_exit);

MODULE_AUTHOR("YinFeng Wang <wangyinfeng@phytium.com>");
MODULE_DESCRIPTION("phytiumlicon ras driver");
MODULE_LICENSE("Dual BSD/GPL");

