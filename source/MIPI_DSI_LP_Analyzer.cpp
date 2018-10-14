#include "MIPI_DSI_LP_Analyzer.h"
#include "MIPI_DSI_LP_AnalyzerSettings.h"
#include <AnalyzerChannelData.h>
#include <fstream>
#include <string>
#include <iostream>

//#define DEBUG

MIPI_DSI_LP_Analyzer::MIPI_DSI_LP_Analyzer()
:	Analyzer2(),  
	mSettings( new MIPI_DSI_LP_AnalyzerSettings() ),
	mSimulationInitialized(false)
{
	SetAnalyzerSettings( mSettings.get() );
	pFile = NULL;
}

MIPI_DSI_LP_Analyzer::~MIPI_DSI_LP_Analyzer()
{
	KillThread();
}

void MIPI_DSI_LP_Analyzer::SetupResults()
{
	mResults.reset( new MIPI_DSI_LP_AnalyzerResults( this, mSettings.get() ) );
	SetAnalyzerResults( mResults.get() );
	mResults->AddChannelBubblesWillAppearOn( mSettings->mPosChannel );
}

#ifdef DEBUG
	#define DEBUG_PRINTF(...)		{fprintf(pFile, __VA_ARGS__);fprintf(pFile, "\n");}
#else
	#define DEBUG_PRINTF(...)		{}
#endif

void MIPI_DSI_LP_Analyzer::WorkerThread()
{
	mSampleRateHz = GetSampleRate();
	sampleStart = 0;
	pulseLength = 0;

	mDataP = GetAnalyzerChannelData( mSettings->mPosChannel );
	mDataN = GetAnalyzerChannelData( mSettings->mNegChannel );

#ifdef DEBUG
	pFile = fopen("log.txt", "w");
#endif

	DEBUG_PRINTF("Log started.");

	for ( ; ; )
	{
		DEBUG_PRINTF("Loop.");
		CheckIfThreadShouldExit();

		if (!GetStart()) {
			mResults->CommitResults();
			continue;
		}

		if (GetBitstream() >= 8) {
			GetData();
		}
	}

#ifdef DEBUG
	fclose(pFile);
#endif
}

bool MIPI_DSI_LP_Analyzer::GetStart()
{
	/* D- needs to be low for start condition. */
	while (mDataN->GetBitState() != BIT_LOW) {
		DEBUG_PRINTF("D- is not low.");
		/* Advance D- and D+. */
		mDataN->AdvanceToNextEdge();
		mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());
	}

	/* Advancing D+ to D- should not cause transition. */
	if (mDataP->WouldAdvancingToAbsPositionCauseTransition(mDataN->GetSampleNumber())) {
		DEBUG_PRINTF("Error: Unexpected D+ transition.");
		/* Advance D+ to error edge. */
		mDataP->AdvanceToNextEdge();
		/* Mark error on D+. */
		mResults->AddMarker(mDataP->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mPosChannel);
		/* Advance D+ to D-. */
		mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());
		return false;
	}

	/* Advance D+ to D-. */
	mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());

	/* D+ should be high. */
	if (mDataP->GetBitState() != BIT_HIGH) {
		DEBUG_PRINTF("Error: D+ is not high.");
		/* Mark error on D+. */
		mResults->AddMarker(mDataP->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mPosChannel);
		/* Advance D- and D+. */
		mDataN->AdvanceToNextEdge();
		mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());
		return false;
	}

	/* Advance D+ to falling edge. */
	mDataP->AdvanceToNextEdge();

	/* Advancing D- to D+ should not cause transition. */
	if (mDataN->WouldAdvancingToAbsPositionCauseTransition(mDataP->GetSampleNumber())) {
		DEBUG_PRINTF("Error: Unexpected D- transition.");
		/* Advance D- to error edge. */
		mDataN->AdvanceToNextEdge();
		/* Mark error on D-. */
		mResults->AddMarker(mDataN->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mNegChannel);
		/* Advance D- to D+. */
		mDataN->AdvanceToAbsPosition(mDataP->GetSampleNumber());
		return false;
	}

	/* Advance D- to D+. */
	mDataN->AdvanceToAbsPosition(mDataP->GetSampleNumber());

	/* D- should be low. */
	if (mDataN->GetBitState() != BIT_LOW) {
		DEBUG_PRINTF("Error: D- is not low.");
		/* Mark error on D-. */
		mResults->AddMarker(mDataN->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mNegChannel);
		/* Advance D-. */
		mDataN->AdvanceToNextEdge();
		return false;
	}

	/* Remember this position as possible start. */
	sampleStart = mDataP->GetSampleNumber();
	DEBUG_PRINTF("Possible start detected. sampleStart = %lld", sampleStart);

	/* Calculate length from start to pulse. */
	U64 startToPulse = mDataN->GetSampleOfNextEdge() - sampleStart;

	/* Go to D- rising edge. */
	mDataN->AdvanceToNextEdge();

	/* Calculate bitrate lenght using D- pulse. */
	pulseLength = mDataN->GetSampleOfNextEdge() - mDataN->GetSampleNumber();

	DEBUG_PRINTF("pulseLength = %lld", pulseLength);

	/* Go to D- falling edge. */
	mDataN->AdvanceToNextEdge();

	/* Check if edge timings are outside boundary. */
	if ((startToPulse <= pulseLength) || (startToPulse > (pulseLength * 5))) {
		DEBUG_PRINTF("Error: D- pulse timing outside boundary.");
		mResults->AddMarker(sampleStart, AnalyzerResults::ErrorX, mSettings->mNegChannel);
		return false;
	}

	/* Check if D+ was low during D- pulse. */
	if (mDataP->WouldAdvancingToAbsPositionCauseTransition(mDataN->GetSampleNumber())) {
		DEBUG_PRINTF("Error: D+ was not low during D- pulse.");
		/* D+ was not low, that's an error. */
		mResults->AddMarker(mDataN->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mNegChannel);
		/* Advance D+ to D-. */
		mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());
		return false;
	}

	/* Timings are ok: we got a start. */
	DEBUG_PRINTF("Start is OK.");
	
	/* Mark start. */
	mResults->AddMarker(sampleStart, AnalyzerResults::Start, mSettings->mPosChannel);

	/* Advance D+ to D- ; we land after D- falling edge on both channels. */
	mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());
	/* Mark D- falling edge. */
	mResults->AddMarker(mDataN->GetSampleNumber(), AnalyzerResults::DownArrow, mSettings->mNegChannel);

	return true;
}

U64 MIPI_DSI_LP_Analyzer::GetBitstream()
{
	U64 bit_counter = 0xFFFF; /* Max number of bits allowed. */

	data.clear();

	/* Get bitstream. */
	while (--bit_counter) {
		DEBUG_PRINTF("GetData() loop. bit_counter = %lld, Dp = %lld , Dm = %lld", bit_counter, mDataP->GetSampleNumber(), mDataN->GetSampleNumber());

		/* Check on which data line we've got next edge. */
		if (mDataP->GetSampleOfNextEdge() <= mDataN->GetSampleOfNextEdge()) {
			/* Check if next edge is too far. */
			if ((mDataP->GetSampleOfNextEdge() - mDataP->GetSampleNumber()) >= (pulseLength * 5)) {
				DEBUG_PRINTF("Error: D+ too far.");
				/* Go to rising edge. */
				mDataP->AdvanceToNextEdge();
				/* Add error marker. */
				mResults->AddMarker(mDataP->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mPosChannel);
				/* Advance D- to D+. */
				mDataN->AdvanceToAbsPosition(mDataP->GetSampleNumber());
				/* Exit bitsteam. */
				break;
			}

			if (mDataN->GetBitState() != BIT_LOW) {

			}

			/* Bit's on D+. */
			Bit bit;
			DEBUG_PRINTF("D+ bit.");
			/* Go to rising edge. */
			mDataP->AdvanceToNextEdge();
			/* Remember bit stats. */
			bit.sampleBegin = mDataP->GetSampleNumber();
			/* Advance D-. */
			mDataN->AdvanceToAbsPosition(bit.sampleBegin);

			/* Check if there are more edges. */
			if (!mDataP->DoMoreTransitionsExistInCurrentData()) {
				bit.sampleEnd = -1; /* Force stop condition detection. */
			} else {
				bit.sampleEnd = mDataP->GetSampleOfNextEdge(); /* Saleae will exit thread if there's no sample further. */
			}

			/* If there are no more transitions on D+ or next D- transition is earlier than D+ transition. */
			if (!mDataP->DoMoreTransitionsExistInCurrentData() || (mDataN->GetSampleOfNextEdge() < mDataP->GetSampleOfNextEdge())) {
				DEBUG_PRINTF("Stop condition (no more D- transitions) @ %lld / data.size() = %lli", bit.sampleBegin, data.size());
				/* This is stop condition, not a data bit. */
				mResults->AddMarker(bit.sampleBegin, AnalyzerResults::Stop, mSettings->mPosChannel);
				/* Advance D- and D+. */
				if (mDataP->DoMoreTransitionsExistInCurrentData() && mDataN->DoMoreTransitionsExistInCurrentData()) {
					mDataN->AdvanceToNextEdge();
					mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());
				}

				/* Exit bitsteam. */
				DEBUG_PRINTF("Exit bitstream.");
				break;
			}

			/* Check if next edge is too far - this could be stop condition. */
			if ((bit.sampleEnd - bit.sampleBegin) >= (pulseLength * 5)) {
				DEBUG_PRINTF("Next edge too far.");

				/* Advance D- to D+. */
				/* TODO: check for transitions. */
				mDataN->AdvanceToAbsPosition(bit.sampleBegin);

				/* Check if D- is low and it's next edge is close. */
				if ((bit.sampleEnd == -1) || ((mDataN->GetBitState()) == BIT_LOW) && ((mDataN->GetSampleOfNextEdge() - mDataN->GetSampleNumber()) <= (pulseLength * 5))) {
					DEBUG_PRINTF("Stop condition (edge too far) @ %lld / data.size() = %lli", bit.sampleBegin, data.size());
					/* This is stop condition, not a data bit. */
					mResults->AddMarker(bit.sampleBegin, AnalyzerResults::Stop, mSettings->mPosChannel);
					/* Advance D- and D+. */
					mDataN->AdvanceToNextEdge();
					mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());

					/* Exit bitsteam. */
					DEBUG_PRINTF("Exit bitstream.");
					break;
				} else {
					mResults->AddMarker(bit.sampleBegin, AnalyzerResults::ErrorX, mSettings->mPosChannel);
					/* Exit bitsteam. */
					break;
				}
			}

			bit.value = BIT_HIGH;
			mResults->AddMarker((bit.sampleBegin >> 1) + (bit.sampleEnd >> 1), AnalyzerResults::One, mSettings->mPosChannel);

			/* Go to falling edge. */
			mDataP->AdvanceToNextEdge();
			/* Save the bit. */
			data.push_back(bit);
			/* Advance D- to bit's end. */
			mDataN->AdvanceToAbsPosition(bit.sampleEnd);
		}
		else {
			/* Check if next edge is too far. */
			if ((mDataN->GetSampleOfNextEdge() - mDataN->GetSampleNumber()) >= (pulseLength * 5)) {
				DEBUG_PRINTF("Error: D- too far.");
				/* Go to rising edge. */
				mDataN->AdvanceToNextEdge();
				/* Add error marker. */
				mResults->AddMarker(mDataN->GetSampleNumber(), AnalyzerResults::ErrorX, mSettings->mNegChannel);
				/* Advance D+ to D-. */
				mDataP->AdvanceToAbsPosition(mDataN->GetSampleNumber());
				/* Exit bitsteam. */
				break;
			}

			/* Bit's on D-. */
			Bit bit;
			DEBUG_PRINTF("D- bit.");
			/* Go to rising edge. */
			mDataN->AdvanceToNextEdge();
			/* Remember bit stats. */
			bit.sampleBegin = mDataN->GetSampleNumber();
			bit.sampleEnd = mDataN->GetSampleOfNextEdge();

			/* Check if next edge is too far. */
			if ((bit.sampleEnd - bit.sampleBegin) >= (pulseLength * 5)) {
				/* Add error marker. */
				mResults->AddMarker(bit.sampleBegin, AnalyzerResults::ErrorX, mSettings->mNegChannel);
				/* Advance D+ to D-. */
				mDataP->AdvanceToAbsPosition(bit.sampleBegin);
				/* Exit bitsteam. */
				break;
			}

			bit.value = BIT_LOW;
			mResults->AddMarker((bit.sampleBegin >> 1) + (bit.sampleEnd >> 1), AnalyzerResults::Zero, mSettings->mNegChannel);

			/* Go to falling edge. */
			mDataN->AdvanceToNextEdge();
			/* Save the bit. */
			data.push_back(bit);
			/* Advance D+ to bit's end. */
			mDataP->AdvanceToAbsPosition(bit.sampleEnd);
		}
	}

	mResults->CommitResults();
	return data.size();
}

U64 MIPI_DSI_LP_Analyzer::GetData(void)
{
	Frame frame;
	U64 byteIndex, byteCount;
	U64 packetType;

	/* Initialize frame's fields. */
	frame.mStartingSampleInclusive = 0;
	frame.mEndingSampleInclusive = 0;
	frame.mData1 = 0;
	frame.mData2 = 0;
	frame.mFlags = 0;
	frame.mType = 0;

	/* Number of bytes in the stream. */
	byteCount = data.size() / 8U;
	byteIndex = 0U;

	if (byteCount == 4U) {
		/* Short packet. */
		packetType = MIPI_DSI_PACKET_SHORT;
	} else
	if (byteCount >= 6U) {
		/* Long packet */
		packetType = MIPI_DSI_PACKET_LONG;
	} else
	{
		/* Unrecognized packet. */
		packetType = MIPI_DSI_PACKET_UNRECOGNIZED;
	}

	/* Extract bitstream by 8 bits. */
	while (data.size() >= 8)
	{
		DataBuilder byteBuilder;
		U64 byte = 0;

		/* Mark first and last samples of a frame. */
		frame.mStartingSampleInclusive = data.front().sampleBegin;
		frame.mEndingSampleInclusive = data.at(7).sampleEnd;

		/* Reset byteBuilder onto byte. */
		byteBuilder.Reset(&byte, AnalyzerEnums::LsbFirst, 8);
		/* Loop through 8 bits of a data. */
		for (int i = 0; i < 8; i++) byteBuilder.AddBit(data.at(i).value);
		/* Remove those 8 bits. */
		data.erase(data.begin(), data.begin() + 8);

		/* Fill frame with data. */
		frame.mData1 = byte;
		frame.mData2 = ((byteCount & UINT32_MAX) << 32U) | (byteIndex & UINT32_MAX);
		frame.mType = (U8)packetType;

		mResults->AddFrame(frame);
		byteIndex++;
	}

	U64 bitsRemaining = data.size(); /* Ideally all bits have been processed. */
	data.clear();
	mResults->CommitResults();

	return bitsRemaining;
}

bool MIPI_DSI_LP_Analyzer::NeedsRerun()
{
	return false;
}

U32 MIPI_DSI_LP_Analyzer::GenerateSimulationData(U64 minimum_sample_index, U32 device_sample_rate, SimulationChannelDescriptor** simulation_channels)
{
	if (mSimulationInitialized == false)
	{
		mSimulationDataGenerator.Initialize(GetSimulationSampleRate(), mSettings.get());
		mSimulationInitialized = true;
	}

	return mSimulationDataGenerator.GenerateSimulationData(minimum_sample_index, device_sample_rate, simulation_channels);
}

U32 MIPI_DSI_LP_Analyzer::GetMinimumSampleRateHz()
{
	return 1000000U; /* 1MHz */
}

const char* MIPI_DSI_LP_Analyzer::GetAnalyzerName() const
{
	return "MIPI DSI LP mode";
}

const char* GetAnalyzerName()
{
	return "MIPI DSI LP mode";
}

Analyzer* CreateAnalyzer()
{
	return new MIPI_DSI_LP_Analyzer();
}

void DestroyAnalyzer( Analyzer* analyzer )
{
	delete analyzer;
}
