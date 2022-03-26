#ifndef _S2500_RAS_H
#define _S2500_RAS_H

#define	FITx_REG	   				0x100
#define DCU_ERR_STATUS_REG			0x200
#define DCU_ERR_LOGEN_REG			0x210
#define DCU_ERR_EN_REG				0x220
#define DCU_INT_EN_REG				0x230
#define DCU_ERR_SET_REG				0x240
#define DCU_ERR_CLR_REG				0x250
#define DCU_DIR_ERR0_REG			0x260
#define DCU_DIR_ERR1_REG			0x270
#define DCU_DIR_ERR2_REG			0x280

#define MCU_ERR_STA_REG				0x1a0
#define MCU_LOG_ENA_REG				0x1b0
#define MCU_ERR_ENA_REG				0x1c0
#define MCU_INT_ENA_REG				0x1d0
#define MCU_ERR_SET_REG				0x1e0
#define MCU_ERR_CLR_REG				0x1f0

#define PEU_ERR_STA_REG				0xc0
#define PEU_ERR_ENA_REG				0xc8

#define LIU_ERR0_FR					0x0
#define LIU_ERR0_CTLR				0x8
#define LIU_ERR0_STATUS				0x10
#define LIU_ERR0_ADDR				0x18
#define LIU_ERR0_MISC0				0x20
#define LIU_ERR0_MISC1				0x28
#define LIU_ERRGSR0					0xe00
#define LIU_ERRFHICR0				0xe80
#define LIU_ERRFHICR1				0xe88
#define LIU_ERRFHICR2				0xe8c
#define LIU_ERRERICR0				0xe90
#define LIU_ERRERICR1				0xe98
#define LIU_ERRERICR2				0xe9c
#define LIU_ERRIRQSR				0xef8

void s2500_print_all_dfx_info(void);

#endif

