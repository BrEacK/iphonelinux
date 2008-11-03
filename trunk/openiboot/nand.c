#include "openiboot.h"
#include "nand.h"
#include "hardware/nand.h"
#include "timer.h"
#include "clock.h"
#include "util.h"

static int banksTable[NAND_NUM_BANKS];

static int NandSetting = 0;
static uint8_t NANDSetting1;
static uint8_t NANDSetting2;
static uint8_t NANDSetting3;
static uint8_t NANDSetting4;
static uint32_t NANDSetting5;
static uint32_t NANDSetting6;
static int NumValidBanks = 0;
static const int NANDBankResetSetting = 1;
static int LargePages;

static NANDData Data;

static uint8_t* aTemporaryReadEccBuf;
static uint8_t* aTemporarySBuf;

#define SECTOR_SIZE 512

static struct UnknownNANDType {
	uint16_t field_0;
	uint16_t field_2;
	uint16_t field_4;
	uint16_t field_6;
	uint16_t field_8;
} Data2;

static const NANDDeviceType SupportedDevices[] = {
	{0x2555D5EC, 8192, 0x80, 4, 64, 4, 2, 4, 2, 7744, 4, 6},
	{0xB614D5EC, 4096, 0x80, 8, 128, 4, 2, 4, 2, 3872, 4, 6},
	{0xB655D7EC, 8192, 0x80, 8, 128, 4, 2, 4, 2, 7744, 4, 6},
	{0xA514D3AD, 4096, 0x80, 4, 64, 4, 2, 4, 2, 3872, 4, 6},
	{0xA555D5AD, 8192, 0x80, 4, 64, 4, 2, 4, 2, 7744, 4, 6},
	{0xA585D598, 8320, 0x80, 4, 64, 6, 2, 4, 2, 7744, 4, 6},
	{0xBA94D598, 4096, 0x80, 8, 216, 6, 2, 4, 2, 3872, 8, 8},
	{0xBA95D798, 8192, 0x80, 8, 216, 6, 2, 4, 2, 7744, 8, 8},
	{0x3ED5D789, 8192, 0x80, 8, 216, 4, 2, 4, 2, 7744, 8, 8},
	{0x3E94D589, 4096, 0x80, 8, 216, 4, 2, 4, 2, 3872, 8, 8},
	{0x3ED5D72C, 8192, 0x80, 8, 216, 4, 2, 4, 2, 7744, 8, 8},
	{0x3E94D52C, 4096, 0x80, 8, 216, 4, 2, 4, 2, 3872, 8, 8},
	{0}
};

static int wait_for_ready(int timeout) {
	if((GET_REG(NAND + NAND_STATUS) & NAND_STATUS_READY) != 0) {
		return 0;
	}

	uint32_t startTime = timer_get_system_microtime();
	while((GET_REG(NAND + NAND_STATUS) & NAND_STATUS_READY) == 0) {
		if(has_elapsed(startTime, timeout * 1000)) {
			return ERROR_TIMEOUT;
		}
	}

	return 0;
}

static int wait_for_status_bit_2(int timeout) {
	if((GET_REG(NAND + NAND_STATUS) & (1 << 2)) != 0) {
		SET_REG(NAND + NAND_STATUS, 1 << 2);
		return 0;
	}

	uint32_t startTime = timer_get_system_microtime();
	while((GET_REG(NAND + NAND_STATUS) & (1 << 2)) == 0) {
		if(has_elapsed(startTime, timeout * 1000)) {
			return ERROR_TIMEOUT;
		}
	}

	SET_REG(NAND + NAND_STATUS, 1 << 2);

	return 0;
}

static int wait_for_status_bit_3(int timeout) {
	if((GET_REG(NAND + NAND_STATUS) & (1 << 3)) != 0) {
		SET_REG(NAND + NAND_STATUS, 1 << 3);
		return 0;
	}

	uint32_t startTime = timer_get_system_microtime();
	while((GET_REG(NAND + NAND_STATUS) & (1 << 3)) == 0) {
		if(has_elapsed(startTime, timeout * 1000)) {
			return ERROR_TIMEOUT;
		}
	}

	SET_REG(NAND + NAND_STATUS, 1 << 3);

	return 0;
}

static int bank_reset_helper(int bank, int timeout) {
	uint32_t startTime = timer_get_system_microtime();
	if(NANDBankResetSetting)
		bank = 0;
	else
		bank &= 0xffff;

	uint32_t toTest = 1 << (bank + 4);

	while((GET_REG(NAND + NAND_STATUS) & toTest) == 0) {
		if(has_elapsed(startTime, timeout * 1000)) {
			return ERROR_TIMEOUT;
		}
	}

	SET_REG(NAND + NAND_STATUS, toTest);

	return 0;
}

static int bank_reset(int bank, int timeout) {
	SET_REG(NAND + NAND_CONFIG,
			((NANDSetting1 & NAND_CONFIG_SETTING1MASK) << NAND_CONFIG_SETTING1SHIFT) | ((NANDSetting2 & NAND_CONFIG_SETTING2MASK) << NAND_CONFIG_SETTING2SHIFT)
			| (1 << (banksTable[bank] + 1)) | NAND_CONFIG_DEFAULTS);

	SET_REG(NAND + NAND_CONFIG2, NAND_CONFIG2_RESET);

	int ret = wait_for_ready(timeout);
	if(ret == 0) {
		ret = bank_reset_helper(bank, timeout);
		udelay(1000);
		return ret;
	} else {
		udelay(1000);
		return ret;
	}
}

int nand_setup() {
	NANDSetting1 = 7;
	NANDSetting2 = 7;
	NANDSetting3 = 7;
	NANDSetting4 = 7;

	bufferPrintf("nand: Probing flash controller...\r\n");

	clock_gate_switch(NAND_CLOCK_GATE1, ON);
	clock_gate_switch(NAND_CLOCK_GATE2, ON);

	int bank;
	for(bank = 0; bank < NAND_NUM_BANKS; bank++) {
		banksTable[bank] = bank;
	}

	NumValidBanks = 0;
	const NANDDeviceType* nandType = NULL;

	SET_REG(NAND + NAND_SETUP, 0);
	SET_REG(NAND + NAND_SETUP, GET_REG(NAND + NAND_SETUP) | (NandSetting << 4));

	for(bank = 0; bank < NAND_NUM_BANKS; bank++) {
		bank_reset(bank, 100);

		SET_REG(NAND + NAND_CON, NAND_CON_SETTING1);
		SET_REG(NAND + NAND_CONFIG,
			((NANDSetting1 & NAND_CONFIG_SETTING1MASK) << NAND_CONFIG_SETTING1SHIFT) | ((NANDSetting2 & NAND_CONFIG_SETTING2MASK) << NAND_CONFIG_SETTING2SHIFT)
			| (1 << (banksTable[bank] + 1)) | NAND_CONFIG_DEFAULTS);

		SET_REG(NAND + NAND_CONFIG2, NAND_CONFIG2_SETTING1);

		wait_for_ready(500);

		SET_REG(NAND + NAND_CONFIG4, 0);
		SET_REG(NAND + NAND_CONFIG3, 0);
		SET_REG(NAND + NAND_CON, (1 << 0));

		wait_for_status_bit_2(500);
		bank_reset_helper(bank, 100);

		SET_REG(NAND + NAND_CONFIG5, (1 << 3));
		SET_REG(NAND + NAND_CON, (1 << 1));

		wait_for_status_bit_3(500);
		uint32_t id = GET_REG(NAND + NAND_ID);
		const NANDDeviceType* candidate = SupportedDevices;
		while(candidate->id != 0) {
			if(candidate->id == id) {
				if(nandType == NULL) {
					nandType = candidate;
				} else if(nandType != candidate) {
					bufferPrintf("nand: Mismatched device IDs (0x%08x after 0x%08x)\r\n", id, nandType->id);
					return ERROR_ARG;
				}
				banksTable[NumValidBanks++] = bank;
			}
			candidate++;
		}

		SET_REG(NAND + NAND_CON, NAND_CON_SETTING1);
	}

	if(nandType == NULL) {
		bufferPrintf("nand: No supported NAND found\r\n");
		return ERROR_ARG;
	}

	Data.DeviceID = nandType->id;

	NANDSetting2 = (((clock_get_frequency(FrequencyBaseBus) * (nandType->NANDSetting2 + 1)) + 99999999)/100000000) - 1;
	NANDSetting1 = (((clock_get_frequency(FrequencyBaseBus) * (nandType->NANDSetting1 + 1)) + 99999999)/100000000) - 1;
	NANDSetting3 = (((clock_get_frequency(FrequencyBaseBus) * (nandType->NANDSetting3 + 1)) + 99999999)/100000000) - 1;
	NANDSetting4 = (((clock_get_frequency(FrequencyBaseBus) * (nandType->NANDSetting4 + 1)) + 99999999)/100000000) - 1;

	if(NANDSetting2 > 7)
		NANDSetting2 = 7;

	if(NANDSetting1 > 7)
		NANDSetting1 = 7;

	if(NANDSetting3 > 7)
		NANDSetting3 = 7;

	if(NANDSetting4 > 7)
		NANDSetting4 = 7;

	Data.blocksPerBank = nandType->blocksPerBank;
	Data.banksTotal = NumValidBanks;
	Data.sectorsPerPage = nandType->sectorsPerPage;
	Data.userSubBlksTotal = nandType->userSubBlksTotal;
	Data.bytesPerSpare = nandType->bytesPerSpare;
	Data.field_2E = 4;
	Data.field_2F = 3;
	Data.pagesPerBlock = nandType->pagesPerBlock;

	if(Data.sectorsPerPage >= 4) {
		LargePages = TRUE;
	} else {
		LargePages = FALSE;
	}

	if(nandType->unk3 == 6) {
		NandSetting = 4;
		NANDSetting5 = Data.sectorsPerPage * 15;
	} else if(nandType->unk3 == 8) {
		NandSetting = 8;
		NANDSetting5 = Data.sectorsPerPage * 20;
	} else if(nandType->unk3 == 4) {
		NandSetting = 0;
		NANDSetting5 = Data.sectorsPerPage * 10;
	}

	if(nandType->unk4 == 6) {
		NANDSetting6 = 4;
	} else if(nandType->unk3 == 8) {
		NANDSetting6 = 8;
	} else if(nandType->unk3 == 4) {
		NANDSetting6 = 0;
	}

	Data.field_4 = 5;
	Data.eccBufSize = SECTOR_SIZE * Data.sectorsPerPage;
	Data.pagesPerBank = Data.pagesPerBlock * Data.blocksPerBank;
	Data.pagesTotal = Data.pagesPerBank * Data.banksTotal;
	Data.pagesPerSubBlk = Data.pagesPerBlock * Data.banksTotal;
	Data.userPagesTotal = Data.userSubBlksTotal * Data.pagesPerSubBlk;
	Data.subBlksTotal = (Data.banksTotal * Data.blocksPerBank) / Data.banksTotal;

	Data2.field_2 = Data.subBlksTotal - Data.userSubBlksTotal - 28;
	Data2.field_4 = 4 + Data2.field_2;
	Data2.field_6 = 3;
	Data2.field_8 = 23;
	if(Data2.field_8 == 0)
		Data.field_22 = 0;

	int bits = 0;
	int i = Data2.field_8;
	while((i <<= 1) != 0) {
		bits++;
	}

	Data.field_22 = bits;

	bufferPrintf("nand: DEVICE: %08x\r\n", Data.DeviceID);
	bufferPrintf("nand: BANKS_TOTAL: %d\r\n", Data.banksTotal);
	bufferPrintf("nand: BLOCKS_PER_BANK: %d\r\n", Data.blocksPerBank);
	bufferPrintf("nand: SUBLKS_TOTAL: %d\r\n", Data.subBlksTotal);
	bufferPrintf("nand: USER_SUBLKS_TOTAL: %d\r\n", Data.userSubBlksTotal);
	bufferPrintf("nand: PAGES_PER_SUBLK: %d\r\n", Data.pagesPerSubBlk);
	bufferPrintf("nand: PAGES_PER_BANK: %d\r\n", Data.pagesPerBank);
	bufferPrintf("nand: SECTORS_PER_PAGE: %d\r\n", Data.sectorsPerPage);
	bufferPrintf("nand: BYTES_PER_SPARE: %d\r\n", Data.bytesPerSpare);

	aTemporaryReadEccBuf = (uint8_t*) malloc(Data.eccBufSize);
	memset(aTemporaryReadEccBuf, 0xFF, SECTOR_SIZE);

	aTemporarySBuf = (uint8_t*) malloc(Data.bytesPerSpare);

	return 0;
}
