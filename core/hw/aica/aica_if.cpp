/*
	aica interface
		Handles RTC, Display mode reg && arm reset reg !
	arm7 is handled on a separate arm plugin now
*/

#include "aica_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/dyna/blockmanager.h"

#include <time.h>

VArray2 aica_ram;
u32 VREG;
u32 ARMRST;
u32 rtc_EN;
int dma_sched_id;
int rtc_schid = -1;

u32 GetRTC_now(void)
{
	// The Dreamcast Epoch time is 1/1/50 00:00 but without support for time zone or DST.
	// We compute the TZ/DST current time offset and add it to the result
	// as if we were in the UTC time zone (as well as the DC Epoch)
	time_t rawtime = time(NULL);
	struct tm localtm, gmtm;
	localtm = *localtime(&rawtime);
	gmtm = *gmtime(&rawtime);
	gmtm.tm_isdst = -1;
	time_t time_offset = mktime(&localtm) - mktime(&gmtm);
	// 1/1/50 to 1/1/70 is 20 years and 5 leap days
	return (20 * 365 + 5) * 24 * 60 * 60 + rawtime + time_offset;
}

u32 ReadMem_aica_rtc(u32 addr,u32 sz)
{
	switch( addr & 0xFF )
	{
	case 0:
		return settings.dreamcast.RTC>>16;
	case 4:
		return settings.dreamcast.RTC &0xFFFF;
	case 8:
		return 0;
	}

	WARN_LOG(AICA, "ReadMem_aica_rtc : invalid address %x sz %d", addr, sz);
	return 0;
}

void WriteMem_aica_rtc(u32 addr,u32 data,u32 sz)
{
	switch( addr & 0xFF )
	{
	case 0:
		if (rtc_EN)
		{
			settings.dreamcast.RTC&=0xFFFF;
			settings.dreamcast.RTC|=(data&0xFFFF)<<16;
			rtc_EN=0;
		}
		return;
	case 4:
		if (rtc_EN)
		{
			settings.dreamcast.RTC&=0xFFFF0000;
			settings.dreamcast.RTC|= data&0xFFFF;
			//TODO: Clean the internal timer ?
		}
		return;
	case 8:
		rtc_EN=data&1;
		return;
	}
}

u32 ReadMem_aica_reg(u32 addr,u32 sz)
{
	addr&=0x7FFF;
	if (sz==1)
	{
		if (addr==0x2C01)
		{
			return VREG;
		}
		else if (addr==0x2C00)
		{
			return ARMRST;
		}
		else
		{
			return libAICA_ReadReg(addr, sz);
		}
	}
	else
	{
		if (addr==0x2C00)
		{
			return (VREG<<8) | ARMRST;
		}
		else
		{
			return libAICA_ReadReg(addr, sz);
		}
	}
}

static void ArmSetRST()
{
	ARMRST&=1;
	arm_SetEnabled(ARMRST==0);
}
void WriteMem_aica_reg(u32 addr,u32 data,u32 sz)
{
	addr&=0x7FFF;

	if (sz==1)
	{
		if (addr==0x2C01)
		{
			VREG=data;
			INFO_LOG(AICA_ARM, "VREG = %02X", VREG);
		}
		else if (addr==0x2C00)
		{
			ARMRST=data;
			INFO_LOG(AICA_ARM, "ARMRST = %02X", ARMRST);
			ArmSetRST();
		}
		else
      {
			libAICA_WriteReg(addr,data,sz);
      }
	}
	else
	{
		if (addr==0x2C00)
		{
			VREG=(data>>8)&0xFF;
			ARMRST=data&0xFF;
			INFO_LOG(AICA_ARM, "VREG = %02X ARMRST %02X", VREG, ARMRST);
			ArmSetRST();
		}
		else
      {
			libAICA_WriteReg(addr,data,sz);
      }
	}
}

static int DreamcastSecond(int tag, int c, int j)
{
	settings.dreamcast.RTC++;

#if FEAT_SHREC != DYNAREC_NONE
	bm_Periodical_1s();
#endif

	return SH4_MAIN_CLOCK;
}

//Init/res/term
void aica_Init()
{
	settings.dreamcast.RTC = GetRTC_now();
	if (rtc_schid == -1)
	{
		rtc_schid = sh4_sched_register(0, &DreamcastSecond);
		sh4_sched_request(rtc_schid, SH4_MAIN_CLOCK);
	}
}

void aica_Reset(bool Manual)
{
	if (!Manual)
      aica_Init();
	VREG = 0;
	ARMRST = 0;
}

void aica_Term()
{

}

static int dma_end_sched(int tag, int cycl, int jitt)
{
	u32 len=SB_ADLEN & 0x7FFFFFFF;
 	if (SB_ADLEN & 0x80000000)
		SB_ADEN = 0;
	else
		SB_ADEN = 1;

 	SB_ADSTAR+=len;
	SB_ADSTAG+=len;
	SB_ADST = 0;	// dma done
	SB_ADLEN = 0;

	// indicate that dma is not happening, or has been paused
 	SB_ADSUSP |= 0x10;

 	asic_RaiseInterrupt(holly_SPU_DMA);

 	return 0;
}

void Write_SB_ADST(u32 addr, u32 data)
{
   //0x005F7800	SB_ADSTAG	RW	AICA:G2-DMA G2 start address 
   //0x005F7804	SB_ADSTAR	RW	AICA:G2-DMA system memory start address 
   //0x005F7808	SB_ADLEN	RW	AICA:G2-DMA length 
   //0x005F780C	SB_ADDIR	RW	AICA:G2-DMA direction 
   //0x005F7810	SB_ADTSEL	RW	AICA:G2-DMA trigger select 
   //0x005F7814	SB_ADEN	RW	AICA:G2-DMA enable 
   //0x005F7818	SB_ADST	RW	AICA:G2-DMA start 
   //0x005F781C	SB_ADSUSP	RW	AICA:G2-DMA suspend 
   if (data&1)
   {
      if (SB_ADEN&1)
      {
         u32 src=SB_ADSTAR;
         u32 dst=SB_ADSTAG;
         u32 len=SB_ADLEN & 0x7FFFFFFF;

         if ((SB_ADDIR&1)==1)
         {
            //swap direction
            u32 tmp=src;
            src=dst;
            dst=tmp;
         }

         WriteMemBlock_nommu_dma(dst,src,len);

         // indicate that dma is in progress
         SB_ADST = 1;
         SB_ADSUSP &= ~0x10;

         // Schedule the end of DMA transfer interrupt
         int cycles = len * (SH4_MAIN_CLOCK / 2 / 25000000);       // 16 bits @ 25 MHz
			if (cycles < 4096)
				dma_end_sched(0, 0, 0);
			else
				sh4_sched_request(dma_sched_id, cycles);
      }
   }

}

void Write_SB_E1ST(u32 addr, u32 data)
{
   //0x005F7800	SB_ADSTAG	RW	AICA:G2-DMA G2 start address 
   //0x005F7804	SB_ADSTAR	RW	AICA:G2-DMA system memory start address 
   //0x005F7808	SB_ADLEN	RW	AICA:G2-DMA length 
   //0x005F780C	SB_ADDIR	RW	AICA:G2-DMA direction 
   //0x005F7810	SB_ADTSEL	RW	AICA:G2-DMA trigger select 
   //0x005F7814	SB_ADEN	RW	AICA:G2-DMA enable 
   //0x005F7818	SB_ADST	RW	AICA:G2-DMA start 
   //0x005F781C	SB_ADSUSP	RW	AICA:G2-DMA suspend 

   if (data&1)
   {
      if (SB_E1EN&1)
      {
         u32 src=SB_E1STAR;
         u32 dst=SB_E1STAG;
         u32 len=SB_E1LEN & 0x7FFFFFFF;

         if (SB_E1DIR==1)
         {
            u32 t=src;
            src=dst;
            dst=t;
            DEBUG_LOG(AICA, "G2-EXT1 DMA : SB_E1DIR==1 DMA Read to 0x%X from 0x%X %d bytes", dst, src, len);
         }
         else
         	DEBUG_LOG(AICA, "G2-EXT1 DMA : SB_E1DIR==0:DMA Write to 0x%X from 0x%X %d bytes", dst, src, len);

         WriteMemBlock_nommu_dma(dst,src,len);

         /*
            for (u32 i=0;i<len;i+=4)
            {
            u32 data=ReadMem32_nommu(src+i);
            WriteMem32_nommu(dst+i,data);
            }*/

         if (SB_E1LEN & 0x80000000)
            SB_E1EN=1;//
         else
            SB_E1EN=0;//

         SB_E1STAR+=len;
         SB_E1STAG+=len;
         SB_E1ST = 0x00000000;//dma done
         SB_E1LEN = 0x00000000;

         asic_RaiseInterrupt(holly_EXT_DMA1);
      }
   }
}

void Write_SB_E2ST(u32 addr, u32 data)
{
	if ((data & 1) && (SB_E2EN & 1))
	{
		u32 src=SB_E2STAR;
		u32 dst=SB_E2STAG;
		u32 len=SB_E2LEN & 0x7FFFFFFF;
 		if (SB_E2DIR==1)
		{
			u32 t=src;
			src=dst;
			dst=t;
			DEBUG_LOG(AICA, "G2-EXT2 DMA : SB_E2DIR==1 DMA Read to 0x%X from 0x%X %d bytes", dst, src, len);
		}
		else
			DEBUG_LOG(AICA, "G2-EXT2 DMA : SB_E2DIR==0:DMA Write to 0x%X from 0x%X %d bytes", dst, src, len);
 		WriteMemBlock_nommu_dma(dst,src,len);
 		if (SB_E2LEN & 0x80000000)
			SB_E2EN=1;
		else
			SB_E2EN=0;
 		SB_E2STAR+=len;
		SB_E2STAG+=len;
		SB_E2ST = 0x00000000;//dma done
		SB_E2LEN = 0x00000000;
 		asic_RaiseInterrupt(holly_EXT_DMA2);
	}
}

void Write_SB_DDST(u32 addr, u32 data)
{
	if ((data & 1) && (SB_DDEN & 1))
	{
		u32 src = SB_DDSTAR;
		u32 dst = SB_DDSTAG;
		u32 len = SB_DDLEN & 0x7FFFFFFF;

		if (SB_DDDIR == 1)
		{
			u32 t = src;
			src = dst;
			dst = t;
			DEBUG_LOG(AICA, "G2-DDev DMA: SB_DDDIR==1 DMA Read to 0x%X from 0x%X %d bytes", dst, src, len);
		}
		else
			DEBUG_LOG(AICA, "G2-DDev DMA: SB_DDDIR==0 DMA Write to 0x%X from 0x%X %d bytes", dst, src, len);

		WriteMemBlock_nommu_dma(dst, src, len);

		if (SB_DDLEN & 0x80000000)
			SB_DDEN = 1;
		else
			SB_DDEN = 0;

		SB_DDSTAR += len;
		SB_DDSTAG += len;
		SB_DDST = 0x00000000;//dma done
		SB_DDLEN = 0x00000000;

		asic_RaiseInterrupt(holly_DEV_DMA);
	}
}

void aica_sb_Init()
{
	//NRM
	//6
	sb_rio_register(SB_ADST_addr,RIO_WF,0,&Write_SB_ADST);

	//I really need to implement G2 dma (and rest dmas actually) properly
	//THIS IS NOT AICA, its G2-EXT (BBA)

	sb_rio_register(SB_E1ST_addr,RIO_WF,0,&Write_SB_E1ST);
   sb_rio_register(SB_E2ST_addr,RIO_WF,0,&Write_SB_E2ST);
	sb_rio_register(SB_DDST_addr,RIO_WF,0,&Write_SB_DDST);

   dma_sched_id = sh4_sched_register(0, &dma_end_sched);
}

void aica_sb_Reset(bool Manual)
{
}

void aica_sb_Term()
{
}
