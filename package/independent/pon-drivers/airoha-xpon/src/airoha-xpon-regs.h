/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_REGS_H_
#define _AIROHA_XPON_REGS_H_

#include <linux/bits.h>

/* AN7581 XG-PON MAC offsets within the 0x1fb65000 window. INT_STATUS and
 * T-CONT/GEM command registers have write side effects.
 */
#define AIROHA_XGPON_SW_RST             0x0000
#define AIROHA_XGPON_MBI_MPI_STOP       0x0004
#define AIROHA_XGPON_VENDOR_ID          0x000c
#define AIROHA_XGPON_VS_SN              0x0010
#define AIROHA_XGPON_ONU_ID             0x0014
#define AIROHA_XGPON_RGS_ID3_0          0x0018
#define AIROHA_XGPON_INT_ENABLE         0x0040
#define AIROHA_XGPON_INT_STATUS         0x0044
#define AIROHA_XGPON_FIFO_ERR_STS       0x0050
#define AIROHA_XGPON_TX_ERR_STS         0x0054
#define AIROHA_XGPON_RX_ERR_STS         0x0058
#define AIROHA_XGPON_O23_O4_PLOAMU_CTRL 0x0100
#define AIROHA_XGPON_ACTIVATION_ST      0x0104
#define AIROHA_XGPON_RSP_TIME           0x0108
#define AIROHA_XGPON_RDM_DLY            0x010c
#define AIROHA_XGPON_EQD                0x0114
#define AIROHA_XGPON_US_PROF_VLD        0x011c
#define AIROHA_XGPON_US_PROF_LEN_01     0x0120
#define AIROHA_XGPON_US_PROF_LEN_23     0x0124
#define AIROHA_XGPON_US_AES_KEY_CTRL    0x0200
#define AIROHA_XGPON_DS_AES_KEY_VLD     0x0204
#define AIROHA_XGPON_AES_UC_IDX0_KEY0   0x0210
#define AIROHA_XGPON_AES_UC_IDX1_KEY0   0x0220
#define AIROHA_XGPON_TCONT_ID_CFG       0x0250
#define AIROHA_XGPON_TCONT_ID_STS       0x0254
#define AIROHA_XGPON_GEM_PORT_CFG       0x0274
#define AIROHA_XGPON_GEM_PORT_STS       0x0278
#define AIROHA_XGPON_PLOAMU_FIFO_STS    0x0300
#define AIROHA_XGPON_PLOAMU_WDATA       0x0304
#define AIROHA_XGPON_PLOAMD_FIFO_STS    0x0308
#define AIROHA_XGPON_PLOAMD_RDATA       0x030c
#define AIROHA_XGPON_KEY_GEN            0x0314
#define AIROHA_XGPON_CUR_KIDX           0x0318
#define AIROHA_XGPON_MSK_0              0x0320
#define AIROHA_XGPON_REGMSK_0           0x0330
#define AIROHA_XGPON_SK_0               0x0340
#define AIROHA_XGPON_HW_GENK_0          0x0350
#define AIROHA_XGPON_PIK0_0             0x0360
#define AIROHA_XGPON_PIK1_0             0x0370
#define AIROHA_XGPON_PIK1_1             0x0374
#define AIROHA_XGPON_PIK1_2             0x0378
#define AIROHA_XGPON_PIK1_3             0x037c
#define AIROHA_XGPON_OIK0_0             0x0380
#define AIROHA_XGPON_OIK1_0             0x0390
#define AIROHA_XGPON_KEK0_0             0x03a0
#define AIROHA_XGPON_KEK1_0             0x03b0
#define AIROHA_XGPON_PON_TAG_0          0x03c0
#define AIROHA_XGPON_PON_TAG_1          0x03c4
#define AIROHA_XGPON_SW_SET_KIDX        0x03e8
#define AIROHA_XGPON_DBG_CAP_SETTING    0x0800
#define AIROHA_XGPON_DBG_BWM_CHK_CTRL   0x0804
#define AIROHA_XGPON_DBG_BWM_CHK_STS    0x0808
#define AIROHA_XGPON_DBG_RESYNC         0x082c
#define AIROHA_XGPON_RX_XGTC_CNT        0x0940
#define AIROHA_XGPON_TX_BURST_CNT       0x0944
#define AIROHA_XGPON_RX_PLOAMD_CNT      0x0950
/* TX_PLOAMU_CNT includes hardware acknowledgements for PLOAMu grants. */
#define AIROHA_XGPON_TX_PLOAMU_CNT     0x0954
#define AIROHA_XGPON_RX_XGEM_CNT       0x0968
#define AIROHA_XGPON_TX_XGEM_CNT       0x096c
#define AIROHA_XGPON_TX_ACK_PLOAMU_CNT 0x0984

/*
 * The first six MAC interrupt bits describe activation. Bit 0 requires
 * software to drain PLOAMd; bits 2–5 reflect hardware O2/3 and O4 replies.
 * Clear only W1C bits owned by this driver.
 */
#define AIROHA_XGPON_INT_PLOAMD_RECV         BIT(0)
#define AIROHA_XGPON_INT_SN_REQUEST          BIT(2)
#define AIROHA_XGPON_INT_SN_SENT             BIT(3)
#define AIROHA_XGPON_INT_RANGING_REQUEST     BIT(4)
#define AIROHA_XGPON_INT_REGISTRATION_SENT   BIT(5)
#define AIROHA_XGPON_INT_BWM_CHECK_ERROR     BIT(13)
#define AIROHA_XGPON_INT_FIFO_ERROR          BIT(15)
#define AIROHA_XGPON_INT_TX_ERROR            BIT(16)
#define AIROHA_XGPON_INT_RX_ERROR            BIT(17)
#define AIROHA_XGPON_INT_KEY_CAL_DONE        BIT(20)
#define AIROHA_XGPON_INT_AES_KEY_SWITCH_DONE BIT(7)
#define AIROHA_XGPON_INT_ACTIVATION_MASK                               \
	(AIROHA_XGPON_INT_PLOAMD_RECV | AIROHA_XGPON_INT_SN_REQUEST |  \
	 AIROHA_XGPON_INT_SN_SENT | AIROHA_XGPON_INT_RANGING_REQUEST | \
	 AIROHA_XGPON_INT_REGISTRATION_SENT)
#define AIROHA_XGPON_INT_ERROR_MASK                                       \
	(AIROHA_XGPON_INT_BWM_CHECK_ERROR | AIROHA_XGPON_INT_FIFO_ERROR | \
	 AIROHA_XGPON_INT_TX_ERROR | AIROHA_XGPON_INT_RX_ERROR)

/* FIFO_ERR_STS[8:7] are W1C; one IRQ can represent multiple MBI RX overruns. */
#define AIROHA_XGPON_FIFO_ERR_RX_MBI_HEADER_OVERRUN  BIT(7)
#define AIROHA_XGPON_FIFO_ERR_RX_MBI_PAYLOAD_OVERRUN BIT(8)

/* MBI connects GDM/CDM; MPI connects the digital PON PHY. Their done bits
 * confirm the respective stop requests.
 */
#define AIROHA_XGPON_MBI_RX_STOP      BIT(0)
#define AIROHA_XGPON_MBI_TX_STOP      BIT(8)
#define AIROHA_XGPON_MBI_RX_STOP_DONE BIT(14)
#define AIROHA_XGPON_MBI_TX_STOP_DONE BIT(15)
#define AIROHA_XGPON_MPI_RX_STOP      BIT(16)
#define AIROHA_XGPON_MPI_TX_STOP      BIT(24)
#define AIROHA_XGPON_MPI_RX_STOP_DONE BIT(30)
#define AIROHA_XGPON_MPI_TX_STOP_DONE BIT(31)
#define AIROHA_XGPON_MBI_MPI_STOP_MASK                         \
	(AIROHA_XGPON_MBI_RX_STOP | AIROHA_XGPON_MBI_TX_STOP | \
	 AIROHA_XGPON_MPI_RX_STOP | AIROHA_XGPON_MPI_TX_STOP)
#define AIROHA_XGPON_MBI_MPI_STOP_DONE_MASK                              \
	(AIROHA_XGPON_MBI_RX_STOP_DONE | AIROHA_XGPON_MBI_TX_STOP_DONE | \
	 AIROHA_XGPON_MPI_RX_STOP_DONE | AIROHA_XGPON_MPI_TX_STOP_DONE)

/* TX_ERR_STS is W1C: burst/SG mismatch, late start, and invalid profile. */
#define AIROHA_XGPON_TX_ERR_BURST_SGL_DIFF  BIT(0)
#define AIROHA_XGPON_TX_ERR_LATE_START      BIT(1)
#define AIROHA_XGPON_TX_ERR_PROFILE_INVALID BIT(2)

/*
 * EN7581 BWmap details are W1C. Low bits 0–13 describe format and timing
 * errors; the upper four bits classify O2/O3/O4/O9 grants.
 */
#define AIROHA_XGPON_BWM_MIN_BURST_INTERVAL         BIT(0)
#define AIROHA_XGPON_BWM_MAX_START_TIME             BIT(1)
#define AIROHA_XGPON_BWM_START_TIME_ORDER           BIT(2)
#define AIROHA_XGPON_BWM_MAX_GRANT_SIZE             BIT(3)
#define AIROHA_XGPON_BWM_MIN_GRANT_SIZE             BIT(4)
#define AIROHA_XGPON_BWM_MY_TCONT_IN_BURST          BIT(5)
#define AIROHA_XGPON_BWM_BURST_SPLIT                BIT(6)
#define AIROHA_XGPON_BWM_ALLOC_HEC_UNCORRECTABLE    BIT(7)
#define AIROHA_XGPON_BWM_OTHER_TCONT_IN_BURST       BIT(13)
#define AIROHA_XGPON_BWM_O2349_NO_PLOAMU_GRANT      BIT(14)
#define AIROHA_XGPON_BWM_O2349_NO_PLOAMU_ONLY_GRANT BIT(15)
#define AIROHA_XGPON_BWM_O49_NO_DEFAULT_TCONT_GRANT BIT(16)
#define AIROHA_XGPON_BWM_O2349_CONTINUOUS_GRANT     BIT(17)

/*
 * Bits 3 and 4 select hardware generation and verification of OMCI MIC with
 * the current OIK. Cleared bits report no_mic in RX descriptors and require
 * software AES-CMAC handling.
 */
#define AIROHA_XGPON_DBG_HW_US_OMCI_MIC  BIT(3)
#define AIROHA_XGPON_DBG_HW_DS_OMCI_MIC  BIT(4)
#define AIROHA_XGPON_DBG_O52_IDLE_ONLY   BIT(8)
#define AIROHA_XGPON_TX_LATE_AUTO_RESYNC BIT(12)
/* DBG_RESYNC[0] and [8] jointly submit TX burst timing resynchronization. */
#define AIROHA_XGPON_SW_RESYNC_START  BIT(0)
#define AIROHA_XGPON_SW_RESYNC_ENABLE BIT(8)
#define AIROHA_XGPON_SW_RESYNC_MASK \
	(AIROHA_XGPON_SW_RESYNC_START | AIROHA_XGPON_SW_RESYNC_ENABLE)

/* TX sync ready permits the external BEN gate to open. */
#define AIROHA_XGPON_TX_SYNC_READY BIT(31)
/* US_PROF_VLD holds four byte-spaced profile-valid bits. */
#define AIROHA_XGPON_US_PROFILE_VALID_MASK 0x01010101
/* SW_SET_KIDX selects PLOAM and OMCI integrity key groups independently. */
#define AIROHA_XGPON_SW_SET_PIK_INDEX  BIT(0)
#define AIROHA_XGPON_SW_SET_OIK_INDEX  BIT(8)
#define AIROHA_XGPON_SW_SET_PIK_ENABLE BIT(16)
#define AIROHA_XGPON_SW_SET_OIK_ENABLE BIT(24)
#define AIROHA_XGPON_CUR_PIK_INDEX     BIT(0)
#define AIROHA_XGPON_CUR_OIK_INDEX     BIT(16)

/* KEY_GEN completion latches W1C INT_STATUS[20]. */
#define AIROHA_XGPON_KEY_GEN_REGMSK   BIT(0)
#define AIROHA_XGPON_KEY_GEN_SK       BIT(1)
#define AIROHA_XGPON_KEY_GEN_OMCI_IK  BIT(2)
#define AIROHA_XGPON_KEY_GEN_PLOAM_IK BIT(3)
#define AIROHA_XGPON_KEY_GEN_KEK      BIT(4)

/*
 * O5 Key_Control/Key_Report negotiate two unicast AES data-key slots. OLT
 * key_index 1/2 map to hardware slot 0/1. DS_AES_KEY_VLD accepts downstream
 * decryption slots; US_AES_KEY_CTRL[0] selects the upstream slot and bit 31
 * enables upstream encryption. PIK/OIK integrity keys use separate selectors.
 */
#define AIROHA_XGPON_DS_AES_UC_IDX0_VALID BIT(0)
#define AIROHA_XGPON_DS_AES_UC_IDX1_VALID BIT(1)
#define AIROHA_XGPON_DS_AES_UC_VALID_MASK GENMASK(1, 0)
#define AIROHA_XGPON_US_AES_KEY_INDEX     BIT(0)
#define AIROHA_XGPON_US_AES_KEY_VALID     BIT(31)

/* Digital PON PHY offsets within the 0x1faf0000 window. */
#define AIROHA_XGPON_PHY_RX_SYNC_CTRL     0x0a04
#define AIROHA_XGPON_PHY_RESET_CTRL       0x0a0c
#define AIROHA_XGPON_PHY_INT_STATUS       0x0a10
#define AIROHA_XGPON_PHY_INT_ENABLE       0x0a14
#define AIROHA_XGPON_PHY_PREAMBLE_BASE    0x0a18
#define AIROHA_XGPON_PHY_DELIMITER_BASE   0x0a38
#define AIROHA_XGPON_PHY_TX_FEC_CTRL      0x0a58
#define AIROHA_XGPON_PHY_PSBU_INFO_BASE   0x0a5c
#define AIROHA_XGPON_PHY_XG_CONTINUE_CTRL 0x0a78
#define AIROHA_XGPON_PHY_DBG_RX_SYNC_ST   0x0b1c
#define AIROHA_XGPON_PHY_SFP_STA          0x0b4c
#define AIROHA_XGPON_PHY_XG_PHY_STA       0x0b54

/*
 * SPI 43 carries digital PON PHY events; SPI 42 carries MAC/PLOAM events.
 * Status bits are W1C. RX_RDY indicates a trainable analog input; SYNC_OK
 * indicates XGTC synchronization.
 */
#define AIROHA_XGPON_PHY_INT_RX_LOS     BIT(0)
#define AIROHA_XGPON_PHY_INT_RX_SYNC_OK BIT(1)
#define AIROHA_XGPON_PHY_INT_RX_LOF     BIT(2)
#define AIROHA_XGPON_PHY_INT_RX_RDY     BIT(9)
#define AIROHA_XGPON_PHY_INT_RX_MASK                                     \
	(AIROHA_XGPON_PHY_INT_RX_LOS | AIROHA_XGPON_PHY_INT_RX_SYNC_OK | \
	 AIROHA_XGPON_PHY_INT_RX_LOF | AIROHA_XGPON_PHY_INT_RX_RDY)

#define AIROHA_XGPON_PHY_RX_ENABLE BIT(16)
/*
 * XG_CONTINUE_CTRL bit 0 selects DBA grant-controlled bursts when clear and
 * continuous test output when set. Normal startup also clears pattern bits
 * [9:8] and all-PRBS bit 16.
 */
#define AIROHA_XGPON_PHY_CONTINUE_ENABLE       BIT(0)
#define AIROHA_XGPON_PHY_CONTINUE_PATTERN_MASK GENMASK(9, 8)
#define AIROHA_XGPON_PHY_CONTINUE_ALL_PRBS     BIT(16)
#define AIROHA_XGPON_PHY_CONTINUE_MASK            \
	(AIROHA_XGPON_PHY_CONTINUE_ENABLE |       \
	 AIROHA_XGPON_PHY_CONTINUE_PATTERN_MASK | \
	 AIROHA_XGPON_PHY_CONTINUE_ALL_PRBS)
/*
 * RX_SYNC_CTRL[10:8] selects digital RX mode 1 for XG-PON.
 */
#define AIROHA_XGPON_PHY_RX_MODE_MASK  GENMASK(10, 8)
#define AIROHA_XGPON_PHY_RX_MODE_XGPON BIT(8)
/*
 * RESET_CTRL[1:0] control active-low resets for SerDes and PHY-D digital
 * logic. A 0-to-3 transition after 1 us resets the SoC receive path.
 */
#define AIROHA_XGPON_PHY_RESET_MASK     GENMASK(1, 0)
#define AIROHA_XGPON_PHY_RESET_RELEASED AIROHA_XGPON_PHY_RESET_MASK
#define AIROHA_XGPON_PHY_RX_SYNC_MASK   GENMASK(1, 0)
#define AIROHA_XGPON_PHY_RX_SYNC_HUNT   0
#define AIROHA_XGPON_PHY_RX_SYNC_PRE    1
#define AIROHA_XGPON_PHY_RX_SYNC_IN     2
#define AIROHA_XGPON_PHY_RX_SYNC_RE     3
#define AIROHA_XGPON_PHY_SFP_RX_LOS     BIT(0)
#define AIROHA_XGPON_PHY_PHYA_READY     BIT(0)

/*
 * SFP_VLD_LEVEL takes the complete IOT register value. EN7572 and compatible
 * 28L95 providers return 0x9 to set LOS/RX_RDY event polarity.
 */
#define AIROHA_XGPON_PHY_SFP_VLD_LEVEL 0x0b48

#define AIROHA_XGPON_MAC_RESET_RELEASED    BIT(0)
#define AIROHA_XGPON_ONU_ID_VALID          BIT(15)
#define AIROHA_XGPON_ONU_ID_MASK           GENMASK(9, 0)
#define AIROHA_XGPON_ACTIVATION_STATE_MASK GENMASK(3, 0)
#define AIROHA_XGPON_RESPONSE_TIME_MASK    GENMASK(13, 0)
#define AIROHA_XGPON_O23_O4_SOFTWARE_REPLY BIT(0)

/*
 * PLOAMd FIFO used counts 32-bit words. A downstream PLOAM occupies 13 words;
 * each RDATA read pops one word.
 */
#define AIROHA_XGPON_PLOAMD_FIFO_OVERRUN   BIT(31)
#define AIROHA_XGPON_PLOAMD_FIFO_USED_MASK GENMASK(7, 0)
#define AIROHA_XGPON_PLOAMD_WORDS          13
#define AIROHA_XGPON_PLOAMD_BYTES          (AIROHA_XGPON_PLOAMD_WORDS * 4)

/*
 * An upstream PLOAM occupies 11 words. FIFO avail uses the same unit; WDATA
 * advances the FIFO. tx_armed serializes whole-message submission.
 */
#define AIROHA_XGPON_PLOAMU_FIFO_OVERRUN    BIT(31)
#define AIROHA_XGPON_PLOAMU_FIFO_AVAIL_MASK GENMASK(7, 0)
#define AIROHA_XGPON_PLOAMU_WORDS           11
#define AIROHA_XGPON_PLOAMU_BYTES           (AIROHA_XGPON_PLOAMU_WORDS * 4)

/* MAC and digital PHY each hold upstream burst profiles indexed 0–3. */
#define AIROHA_XGPON_PROFILE_VALID(_n) BIT((_n) * 8)
#define AIROHA_XGPON_PROFILE_VERSION_MASK(_n) \
	GENMASK((_n) * 8 + 7, (_n) * 8 + 4)
#define AIROHA_XGPON_PHY_PROFILE_STRIDE      0x8
#define AIROHA_XGPON_PHY_PSBU_STRIDE         0x4
#define AIROHA_XGPON_PHY_FEC_ENABLE(_n)      BIT((_n) * 8)
#define AIROHA_XGPON_PHY_PSBU_DELIMITER_MASK GENMASK(3, 0)
#define AIROHA_XGPON_PHY_PSBU_PREAMBLE_MASK  GENMASK(11, 8)
#define AIROHA_XGPON_PHY_PSBU_REPEAT_MASK    GENMASK(23, 16)

/* The T-CONT table holds 32 indices; MAC Alloc-IDs are 14 bits. */
#define AIROHA_XGPON_TCONT_CMD_WRITE  BIT(31)
#define AIROHA_XGPON_TCONT_INDEX_MASK GENMASK(24, 20)
#define AIROHA_XGPON_TCONT_VALID      BIT(16)
#define AIROHA_XGPON_ALLOC_ID_MASK    GENMASK(13, 0)
#define AIROHA_XGPON_TCONT_CMD_DONE   BIT(31)

/* GEM Port IDs are 16 bits. Command bit 31 starts a write; status bit 31
 * signals completion.
 */
#define AIROHA_XGPON_GEM_CMD_WRITE  BIT(31)
#define AIROHA_XGPON_GEM_VALID      BIT(18)
#define AIROHA_XGPON_GEM_UNICAST    BIT(17)
#define AIROHA_XGPON_GEM_US_ENCRYPT BIT(16)
#define AIROHA_XGPON_GEM_ID_MASK    GENMASK(15, 0)
#define AIROHA_XGPON_GEM_CMD_DONE   BIT(31)

#endif
