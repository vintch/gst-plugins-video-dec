#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <linux/videodev2.h>

#include "decoder.h"
#include "gstnxvideodec.h"

static gint AVCDecodeFrame( NX_VIDEO_DEC_STRUCT *pNxVideoDecHandle, GstBuffer *pGstBuf, NX_V4L2DEC_OUT *pDecOut );
static gint Mpeg2DecodeFrame( NX_VIDEO_DEC_STRUCT *pNxVideoDecHandle, GstBuffer *pGstBuf, NX_V4L2DEC_OUT *pDecOut );
static gint Mpeg4DecodeFrame( NX_VIDEO_DEC_STRUCT *pNxVideoDecHandle, GstBuffer *pGstBuf, NX_V4L2DEC_OUT *pDecOut );
static gint ParseH264Info( guint8 *pData, gint size, NX_AVCC_TYPE *pH264Info );
static gint InitializeCodaVpu( NX_VIDEO_DEC_STRUCT *pHDec, guint8 *pInitBuf, gint initBufSize );
static gint FlushDecoder( NX_VIDEO_DEC_STRUCT *pNxVideoDecHandle );
//TimeStamp
static void InitVideoTimeStamp( NX_VIDEO_DEC_STRUCT *hDec);
static void PushVideoTimeStamp( NX_VIDEO_DEC_STRUCT *hDec, gint64 timestamp, guint flag );
static gint PopVideoTimeStamp( NX_VIDEO_DEC_STRUCT *hDec, gint64 *pTimestamp, guint *pFlag );

//
//			Find Codec Matching Codec Information
//
gint FindCodecInfo( GstVideoCodecState *pState, NX_VIDEO_DEC_STRUCT *pDecHandle )
{
	guint codecType = -1;
	GstStructure *pStructure = gst_caps_get_structure( pState->caps, 0 );;
	const gchar *pMime = gst_structure_get_name( pStructure );

	FUNC_IN();

	pDecHandle->width  = GST_VIDEO_INFO_WIDTH( &pState->info );
	pDecHandle->height = GST_VIDEO_INFO_HEIGHT( &pState->info );
	pDecHandle->fpsNum = GST_VIDEO_INFO_FPS_N( &pState->info );
	pDecHandle->fpsDen = GST_VIDEO_INFO_FPS_D( &pState->info );

	if( 0 == pDecHandle->fpsNum )
	{
		pDecHandle->fpsNum = 30;
		pDecHandle->fpsDen = 1;
	}

	g_print("mime type = %s\n", pMime);

	// H.264
	if( !strcmp(pMime, "video/x-h264") )
	{
		codecType = V4L2_PIX_FMT_H264;
	}
	// H.263
	else if( !strcmp(pMime, "video/x-h263") )
	{
		codecType = V4L2_PIX_FMT_H263;
	}
	// xvid
	else if( !strcmp(pMime, "video/x-xvid" ) )
	{
		codecType = V4L2_PIX_FMT_MPEG4;
	}
	// mpeg 2 & 4
	else if( !strcmp(pMime, "video/mpeg") )
	{
		gint tmpVer=0;
		gst_structure_get_int( pStructure, "mpegversion", &tmpVer );
		if( tmpVer == 2 )
		{
			codecType = V4L2_PIX_FMT_MPEG2;
		}
		else if( tmpVer == 4 )
		{
			codecType = V4L2_PIX_FMT_MPEG4;
		}
	}

	if( codecType == -1 )
	{
		g_print("out of profile or not supported video codec.(mime_type=%s)\n", pMime);
	}

	if( pDecHandle->width > NX_MAX_WIDTH || pDecHandle->height > NX_MAX_HEIGHT )
		goto error_outofrange;

	FUNC_OUT();

	return codecType;

error_outofrange:
	g_print("out of resolution for %s.(Max %dx%d, In %dx%d )\n", pMime, NX_MAX_WIDTH, NX_MAX_HEIGHT, pDecHandle->width, pDecHandle->height);
	return -1;
}

gboolean GetExtraInfo( NX_VIDEO_DEC_STRUCT *pDecHandle, guint8 *pCodecData, gint codecDataSize )
{
	if ( codecDataSize > 0 && pCodecData )
	{
		if( pDecHandle->codecType == V4L2_PIX_FMT_H264 )
		{
			if( pDecHandle->pH264Info )
			{
				g_free(pDecHandle->pH264Info);
			}
			pDecHandle->pH264Info = (NX_AVCC_TYPE*)g_malloc( sizeof(NX_AVCC_TYPE) );
			memset( pDecHandle->pH264Info, 0, sizeof(NX_AVCC_TYPE) );
			// H264(AVC)
			if( ParseH264Info( pCodecData, codecDataSize, pDecHandle->pH264Info ) != 0 )
			{
				g_printerr( "Error unsupported h264 stream!\n" );
				return FALSE;
			}
			else
			{
				// Debugging
				g_print( "NumSps = %d, NumPps = %d, type = %s\n",
					pDecHandle->pH264Info->numSps,
					pDecHandle->pH264Info->numPps,
					(pDecHandle->pH264Info->eStreamType==NX_H264_STREAM_AVCC)?"avcC type":"AnnexB type");
			}
		}
		else if( pDecHandle->codecType == V4L2_PIX_FMT_H263 )
		{
			// H.263
			memcpy( pDecHandle->pExtraData, pCodecData, codecDataSize );
		}
		else if( pDecHandle->codecType == V4L2_PIX_FMT_MPEG2 )
		{
			// mpeg 2
			memcpy( pDecHandle->pExtraData, pCodecData, codecDataSize );
		}
		else if( pDecHandle->codecType == V4L2_PIX_FMT_MPEG4 )
		{
			// mpeg  4
			memcpy( pDecHandle->pExtraData, pCodecData, codecDataSize );
		}
	}
	else
	{
		g_printerr( "Codec_data not exist.\n" );
	}

	return TRUE;
}

NX_VIDEO_DEC_STRUCT *OpenVideoDec()
{
	NX_VIDEO_DEC_STRUCT *pDecHandle = NULL;

	FUNC_IN();

	pDecHandle = g_malloc(sizeof(NX_VIDEO_DEC_STRUCT));

	if( NULL == pDecHandle )
	{
		g_printerr("%s(%d) Create VideoHandle failed.\n", __FILE__, __LINE__);
		return NULL;
	}

	memset (pDecHandle, 0 ,sizeof(NX_VIDEO_DEC_STRUCT));

	FUNC_OUT();

	return pDecHandle;
}

gint InitVideoDec( NX_VIDEO_DEC_STRUCT *pDecHandle )
{
	gint ret = 0;
	FUNC_IN();

	pDecHandle->hCodec = NX_V4l2DecOpen( pDecHandle->codecType );
	if ( NULL == pDecHandle->hCodec )
	{
		g_printerr("%s(%d) NX_V4l2DecOpen() failed.\n", __FILE__, __LINE__);
		return -1;
	}

	if ( V4L2_PIX_FMT_H264 == pDecHandle->codecType )
	{
		gint MBs = ((pDecHandle->width+15)>>4)*((pDecHandle->height+15)>>4);
		// Under 720p
		if ( MBs <= ((1280>>4)*(720>>4)) )
		{
			pDecHandle->bufferCountActual = VID_OUTPORT_MIN_BUF_CNT_H264_UNDER720P;
		}
		// 1080p
		else
		{
			pDecHandle->bufferCountActual = VID_OUTPORT_MIN_BUF_CNT_H264_1080P;
		}
	}
	else
	{
		pDecHandle->bufferCountActual = VID_OUTPORT_MIN_BUF_CNT;
	}

	pDecHandle->pTmpStrmBuf = g_malloc(MAX_INPUT_BUF_SIZE);
	pDecHandle->tmpStrmBufSize = MAX_INPUT_BUF_SIZE;

	switch ( pDecHandle->codecType )
	{
		case V4L2_PIX_FMT_H264:
			pDecHandle->DecodeFrame = AVCDecodeFrame;
			break;
		case V4L2_PIX_FMT_MPEG2:
			pDecHandle->DecodeFrame = Mpeg2DecodeFrame;
			break;
		case V4L2_PIX_FMT_MPEG4:
			pDecHandle->DecodeFrame = Mpeg4DecodeFrame;
			break;
		case V4L2_PIX_FMT_H263:
			pDecHandle->DecodeFrame = Mpeg4DecodeFrame;
			break;
	}

	InitVideoTimeStamp(pDecHandle);

	FUNC_OUT();

	return ret;
}

gint VideoDecodeFrame(NX_VIDEO_DEC_STRUCT *pDecHandle, GstBuffer *pInGstBuf, NX_V4L2DEC_OUT *pOutDecOut)
{
	FUNC_IN();
	gint ret = -1;

	if( pDecHandle->hCodec && pDecHandle->DecodeFrame )
	{
		ret = pDecHandle->DecodeFrame( pDecHandle, pInGstBuf, pOutDecOut );
	}
	else
	{
		return -1;
	}

	FUNC_OUT();

	return ret;
}

void CloseVideoDec( NX_VIDEO_DEC_STRUCT *pDecHandle )
{
	if( pDecHandle->hCodec )
	{
		NX_V4l2DecClose( pDecHandle->hCodec );
		pDecHandle->hCodec = NULL;
	}

	if( pDecHandle->pExtraData )
	{
		g_free(pDecHandle->pExtraData);
		pDecHandle->pExtraData = NULL;
		pDecHandle->extraDataSize = 0;
	}

	if( pDecHandle->pH264Info )
	{
		g_free( pDecHandle->pH264Info );
		pDecHandle->pH264Info = NULL;
	}

	if (pDecHandle->pTmpStrmBuf)
	{
		g_free(pDecHandle->pTmpStrmBuf);
		pDecHandle->pTmpStrmBuf = NULL;
	}

	if(pDecHandle)
	{
		g_free(pDecHandle);
	}
}

gint DisplayDone( NX_VIDEO_DEC_STRUCT *pDecHandle, gint v4l2BufferIdx )
{
	FUNC_IN();

	if( pDecHandle->hCodec && (v4l2BufferIdx >= 0) )
	{
		NX_V4l2DecClrDspFlag( pDecHandle->hCodec, NULL, v4l2BufferIdx );
		VDecSemPost( pDecHandle->pSem );
	}

	FUNC_OUT();

	return 0;
}

gint GetTimeStamp(NX_VIDEO_DEC_STRUCT *pDecHandle, gint64 *pTimestamp)
{
	gint ret = 0;
	guint flag;

	PopVideoTimeStamp(pDecHandle, pTimestamp, &flag );

	return ret;
}

// Copy Image YV12 to General YV12
gint CopyImageToBufferYV12( uint8_t *pSrcY, uint8_t *pSrcU, uint8_t *pSrcV, uint8_t *pDst, uint32_t strideY, uint32_t strideUV, uint32_t width, uint32_t height )
{
	uint32_t i;
	if( width == strideY )
	{
		memcpy( pDst, pSrcY, width*height );
		pDst += width*height;
	}
	else
	{
		for( i=0 ; i<height ; i++ )
		{
			memcpy( pDst, pSrcY, width );
			pSrcY += strideY;
			pDst += width;
		}
	}

	width /= 2;
	height /= 2;
	if( width == strideUV )
	{
		memcpy( pDst, pSrcU, width*height );
		pDst += width*height;
		memcpy( pDst, pSrcV, width*height );
	}
	else
	{
		for( i=0 ; i<height ; i++ )
		{
			memcpy( pDst, pSrcU, width );
			pSrcY += strideY;
			pDst += width;
		}
		for( i=0 ; i<height ; i++ )
		{
			memcpy( pDst, pSrcV, width );
			pSrcY += strideY;
			pDst += width;
		}
	}
	return 0;
}

static gint FlushDecoder( NX_VIDEO_DEC_STRUCT *pDecHandle )
{

	FUNC_IN();

	InitVideoTimeStamp(pDecHandle);

	if( pDecHandle->hCodec )
	{
		NX_V4l2DecFlush( pDecHandle->hCodec );
	}

	FUNC_OUT();

	return 0;
}

static gint InitializeCodaVpu(NX_VIDEO_DEC_STRUCT *pHDec, guint8 *pSeqInfo, gint seqInfoSize )
{
	gint ret = -1;

	FUNC_IN();

	if( pHDec->hCodec )
	{
		NX_V4L2DEC_SEQ_IN seqIn;
		NX_V4L2DEC_SEQ_OUT seqOut;
		memset( &seqIn, 0, sizeof(seqIn) );
		memset( &seqOut, 0, sizeof(seqOut) );
		seqIn.width   = pHDec->width;
		seqIn.height  = pHDec->height;
		seqIn.seqBuf = pSeqInfo;
		seqIn.seqSize = seqInfoSize;

		if ( 0 != (ret = NX_V4l2DecParseVideoCfg( pHDec->hCodec, &seqIn, &seqOut )) )
		{
			g_print("%s : NX_V4l2DecParseVideoCfg() is failed!!\n", __func__);
			return ret;
		}

		seqIn.width = seqOut.width;
		seqIn.height = seqOut.height;
		seqIn.numBuffers = pHDec->bufferCountActual;
		seqIn.imgPlaneNum = pHDec->imgPlaneNum;
		seqIn.imgFormat = seqOut.imgFourCC;
		ret = NX_V4l2DecInit( pHDec->hCodec, &seqIn );

		pHDec->minRequiredFrameBuffer = seqOut.minBuffers;
		pHDec->pSem = VDecSemCreate( pHDec->bufferCountActual );
		g_print("<<<<<<<<<< InitializeCodaVpu(Min=%d, %dx%d) (ret = %d) >>>>>>>>>\n",
			pHDec->minRequiredFrameBuffer, seqOut.width, seqOut.height, ret );
	}

	FUNC_OUT();

	return ret;
}

//
//
//								H.264 Decoder
//

//
//			avcC format
//	Name					Bits		Descriptions
//	===============================================
//	CFG version				8 bits		"1"
// 	AVC porfile indication	8 bits		Profile code
//	Profile compatibility	8 bits		Compatible profile
//	AVC level indication	8 bits		Level code
//	Reserved				6 bits		0b111111
//	Length size minus one	2 bits		Nal unit length size
//	Reserved				3 bits		0b111
// 	Num of SPS				5 bits		Number of SPS
//	SPS length				16bits		SPS length N
//	SPS Data				N byts		SPS data
//	Num of PPS				8 bits		Number of PPS
//	PPS length				16bits		PPS length M
//	PPS Data				M Byts		PPS data
//
static gint ParseSpsPpsFromAVCC( unsigned char *pExtraData, gint extraDataSize, NX_AVCC_TYPE *pH264Info )
{

	gint length, i, pos=0;

	FUNC_IN();

	if( 1!=pExtraData[0] || 11>extraDataSize )
	{
		g_printerr( "Error : Invalid \"avcC\" data(%d)\n", extraDataSize );
		return -1;
	}

	// Parser "avcC" format data
	pos++; // Skip Version
	pH264Info->profileIndication	= pExtraData[pos];			pos++;
	pH264Info->compatibleProfile	= pExtraData[pos];			pos++;
	pH264Info->levelIndication		= pExtraData[pos];			pos++;
	pH264Info->nalLengthSize		= (pExtraData[pos]&0x03)+1;	pos++;

	if( 100 < pH264Info->profileIndication  )
	{
		g_printerr( "H264 profile too high!(%d)\n", pH264Info->profileIndication );
		return -1;
	}

	// parser spsp
	pH264Info->spsppsSize = 0;
	pH264Info->numSps = (pExtraData[pos] & 0x1f);	pos++;

	for( i=0 ; i<pH264Info->numSps ; i++ )
	{
		length = (pExtraData[pos]<<8)|pExtraData[pos+1];
		pos+=2;
		if( (pos+length) > extraDataSize )
		{
			g_printerr( "extraData size too small(SPS)\n" );
			return -1;
		}
		pH264Info->spsppsData[pH264Info->spsppsSize+0] = 0;
		pH264Info->spsppsData[pH264Info->spsppsSize+1] = 0;
		pH264Info->spsppsData[pH264Info->spsppsSize+2] = 0;
		pH264Info->spsppsData[pH264Info->spsppsSize+3] = 1;
		pH264Info->spsppsSize += 4;
		memcpy( pH264Info->spsppsData + pH264Info->spsppsSize, pExtraData + pos, length );
		pH264Info->spsppsSize += length;
		pos += length;
	}

	// parse pps
	pH264Info->numPps = pExtraData[pos];			pos++;
	for( i=0 ; i<pH264Info->numPps ; i++ )
	{
		length = (pExtraData[pos]<<8)|pExtraData[pos+1];
		pos+=2;
		if( (pos+length) > extraDataSize )
		{
			g_printerr( "extraData size too small(PPS)\n" );
			return -1;
		}
		pH264Info->spsppsData[pH264Info->spsppsSize+0] = 0;
		pH264Info->spsppsData[pH264Info->spsppsSize+1] = 0;
		pH264Info->spsppsData[pH264Info->spsppsSize+2] = 0;
		pH264Info->spsppsData[pH264Info->spsppsSize+3] = 1;
		pH264Info->spsppsSize += 4;
		memcpy( pH264Info->spsppsData + pH264Info->spsppsSize, pExtraData + pos, length );
		pH264Info->spsppsSize += length;
		pos += length;
	}

	if( 1>pH264Info->numSps || 1>pH264Info->numPps )
	{
		return -1;
	}

	FUNC_OUT();

	return 0;
}

static gint ParseH264Info( guint8 *pData, gint size, NX_AVCC_TYPE *pH264Info )
{
	FUNC_IN();

	if( size <= 0 )
	{
		return -1;
	}

	if( pData[0] == 0 )
	{
		pH264Info->eStreamType = NX_H264_STREAM_ANNEXB;
		memcpy( pH264Info->spsppsData, pData, size );
		pH264Info->spsppsSize = size;
	}
	else
	{
		pH264Info->eStreamType = NX_H264_STREAM_AVCC;
		return ParseSpsPpsFromAVCC( pData, size, pH264Info );
	}

	FUNC_OUT();

	return 0;
}

static gint ParseAvcStream( guint8 *pInBuf, gint inSize, gint nalLengthSize, unsigned char *pBuffer, gint outBufSize, gint *pIsKey )
{

	int nalLength;
	int pos=0;
	*pIsKey = 0;

	FUNC_IN();

	// 'avcC' format
	do{
		nalLength = 0;
		if( nalLengthSize == 4 )
		{
			nalLength = pInBuf[0]<<24 | pInBuf[1]<<16 | pInBuf[2]<<8 | pInBuf[3];
		}
		else if( nalLengthSize == 2 )
		{
			nalLength = pInBuf[0]<< 8 | pInBuf[1];
		}
		else if( nalLengthSize == 3 )
		{
			nalLength = pInBuf[0]<<16 | pInBuf[1]<<8  | pInBuf[2];
		}
		else if( nalLengthSize == 1 )
		{
			nalLength = pInBuf[0];
		}

		pInBuf  += nalLengthSize;
		inSize -= nalLengthSize;

		if( 0==nalLength || inSize<(int)nalLength )
		{
			g_print("Error : avcC type nal length error (nalLength = %d, inSize=%d, nalLengthSize=%d)\n", nalLength, inSize, nalLengthSize);
			return -1;
		}

		/* put nal start code */
		pBuffer[pos + 0] = 0x00;
		pBuffer[pos + 1] = 0x00;
		pBuffer[pos + 2] = 0x00;
		pBuffer[pos + 3] = 0x01;
		pos += 4;

		if( (pInBuf[0] & 0x1f ) == 0x5 )
		{
			*pIsKey = 1;
		}

		memcpy( pBuffer + pos, pInBuf, nalLength );
		pos += nalLength;

		inSize -= nalLength;
		pInBuf += nalLength;
	}while( 2<inSize );

	FUNC_OUT();

	return pos;
}

static gint AVCDecodeFrame( NX_VIDEO_DEC_STRUCT *pNxVideoDecHandle, GstBuffer *pGstBuf, NX_V4L2DEC_OUT *pDecOut )
{
	NX_VIDEO_DEC_STRUCT *pHDec = pNxVideoDecHandle;
	guint8 *pInBuf = NULL;
	GstMapInfo mapInfo;
	gint inSize = 0;
	NX_AVCC_TYPE *h264Info = NULL;
	gint isKey = 0;
	guint8 *pDecBuf = NULL;
	gint decBufSize = 0;
	gint ret = 0;
	gint64 timestamp = 0;
	NX_V4L2DEC_IN decIn;

	FUNC_IN();

	if( pHDec->bFlush )
	{
		FlushDecoder( pHDec );
		pHDec->bFlush = FALSE;
	}

	h264Info = pHDec->pH264Info;
	gst_buffer_map(pGstBuf, &mapInfo, GST_MAP_READ);
	pInBuf = mapInfo.data;
	inSize = gst_buffer_get_size(pGstBuf);

	// Push Input Time Stamp
	if ( GST_BUFFER_PTS_IS_VALID(pGstBuf) )
	{
		PushVideoTimeStamp(pHDec, GST_BUFFER_PTS(pGstBuf), GST_BUFFER_FLAGS(pGstBuf) );
		timestamp = GST_BUFFER_PTS(pGstBuf);
	}
	else if ( GST_BUFFER_DTS_IS_VALID(pGstBuf) )
	{
		PushVideoTimeStamp(pHDec, GST_BUFFER_DTS(pGstBuf), GST_BUFFER_FLAGS(pGstBuf) );
		timestamp = GST_BUFFER_DTS(pGstBuf);
	}

	if( FALSE == pHDec->bInitialized )
	{
		pDecBuf = pHDec->pTmpStrmBuf;

		if( NULL == h264Info ) //h264Info is Seqence data
		{
			memcpy( pDecBuf, pInBuf, inSize );
			decBufSize = inSize;
		}
		else
		{
			// AVCC Type
			if( h264Info->eStreamType == NX_H264_STREAM_AVCC )
			{
				memcpy( pDecBuf, h264Info->spsppsData, h264Info->spsppsSize );
				decBufSize = h264Info->spsppsSize;
				decBufSize += ParseAvcStream( pInBuf, inSize, h264Info->nalLengthSize, pDecBuf+decBufSize, MAX_INPUT_BUF_SIZE, &isKey );
			}
			// Annex B Type
			else
			{
				memcpy( pDecBuf, h264Info->spsppsData, h264Info->spsppsSize );
				decBufSize = h264Info->spsppsSize;
				memcpy( pDecBuf+decBufSize, pInBuf, inSize );
				decBufSize += inSize;
			}
		}

		// Initialize VPU
		ret = InitializeCodaVpu(pHDec, pDecBuf, decBufSize );

		if( 0 > ret )
		{
			g_print("VPU initialized Failed!!!!\n");
			NX_V4l2DecClose( pNxVideoDecHandle->hCodec );
			pNxVideoDecHandle->hCodec = NULL;
			goto AVCDecode_Exit;
		}

		pHDec->bInitialized = TRUE;

		decIn.strmBuf = pDecBuf;
		decIn.strmSize = inSize;
		decIn.timeStamp = timestamp;
		decIn.eos = 0;
		VDecSemPend(pHDec->pSem);
		ret = NX_V4l2DecDecodeFrame( pHDec->hCodec,&decIn, pDecOut );
		if( (0 != ret ) || (0 > pDecOut->dispIdx) )
		{
			VDecSemPost( pHDec->pSem );
		}
	}
	else
	{
		if( (h264Info) && (h264Info->eStreamType == NX_H264_STREAM_AVCC) )
		{
			pDecBuf = pHDec->pTmpStrmBuf;
			decBufSize = ParseAvcStream( pInBuf, inSize, h264Info->nalLengthSize, pDecBuf+decBufSize, MAX_INPUT_BUF_SIZE, &isKey );
		}
		// Annex B Type
		else
		{
			pDecBuf = pInBuf;
			decBufSize = inSize;
		}

		decIn.strmBuf = pDecBuf;
		decIn.strmSize = decBufSize;
		decIn.timeStamp = timestamp;
		decIn.eos = 0;
		VDecSemPend(pHDec->pSem);
		ret = NX_V4l2DecDecodeFrame( pHDec->hCodec,&decIn, pDecOut );
		if( (0 != ret ) || (0 > pDecOut->dispIdx) )
		{
			VDecSemPost( pHDec->pSem );
		}
	}

AVCDecode_Exit:
	gst_buffer_unmap (pGstBuf,&mapInfo);

	FUNC_OUT();

	return ret;
}
//
//								End of the H.264 Decoder
//
//////////////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////////////
//
//							MPEG2 Decoder
//

#define		MPEG2_PICTURE_START_CODE		0x00000100
#define		MPEG2_SEQUENCE_HEADER			0x000001B3
static gint Mpeg2DecodeFrame( NX_VIDEO_DEC_STRUCT *pNxVideoDecHandle, GstBuffer *pGstBuf, NX_V4L2DEC_OUT *pDecOut )
{

	GstMapInfo mapInfo;
	guint8 *pInBuf = NULL;
	gint inSize = 0;
	guint8 *pDecBuf = NULL;
	gint decBufSize = 0;
	gint ret=0;
	NX_V4L2DEC_IN decIn;
	gint64 timestamp = 0;
	NX_VIDEO_DEC_STRUCT *pHDec = pNxVideoDecHandle;

	FUNC_IN();

	if( pHDec->bFlush )
	{
		FlushDecoder( pHDec );
		pHDec->bFlush = FALSE;
	}

	gst_buffer_map(pGstBuf, &mapInfo, GST_MAP_READ);
	pInBuf = mapInfo.data;
	inSize = gst_buffer_get_size(pGstBuf);

	// Push Input Time Stamp
	if ( GST_BUFFER_PTS_IS_VALID(pGstBuf) )
	{
		PushVideoTimeStamp(pHDec, GST_BUFFER_PTS(pGstBuf), GST_BUFFER_FLAGS(pGstBuf) );
		timestamp = GST_BUFFER_PTS(pGstBuf);
	}
	else if ( GST_BUFFER_DTS_IS_VALID(pGstBuf) )
	{
		PushVideoTimeStamp(pHDec, GST_BUFFER_DTS(pGstBuf), GST_BUFFER_FLAGS(pGstBuf) );
		timestamp = GST_BUFFER_DTS(pGstBuf);
	}

	if( FALSE == pHDec->bInitialized )
	{
		pDecBuf = pHDec->pTmpStrmBuf;

		if( pHDec->extraDataSize  )
		{
			memcpy( pDecBuf, pHDec->pExtraData, pHDec->extraDataSize );
			decBufSize = pHDec->extraDataSize;
			memcpy( pDecBuf+decBufSize, pInBuf, inSize );
			decBufSize += inSize;
		}
		else
		{
			memcpy( pDecBuf, pInBuf, inSize );
			decBufSize = inSize;
		}

		// Initialize VPU
		ret = InitializeCodaVpu(pHDec, pDecBuf, decBufSize );

		if( 0 > ret )
		{
			g_print("VPU initialized Failed!!!!\n");
			NX_V4l2DecClose( pNxVideoDecHandle->hCodec );
			pNxVideoDecHandle->hCodec = NULL;
			goto Mpeg2Decode_Exit;

		}

		pHDec->bInitialized = TRUE;

		ret = 0;
		pDecOut->dispIdx = -1;
	}
	else
	{

		decIn.strmBuf = pInBuf;
		decIn.strmSize = inSize;
		decIn.timeStamp = timestamp;
		decIn.eos = 0;
		VDecSemPend(pHDec->pSem);
		ret = NX_V4l2DecDecodeFrame( pHDec->hCodec,&decIn, pDecOut );
		if( (0 != ret ) || (0 > pDecOut->dispIdx) )
		{
			VDecSemPost( pHDec->pSem );
		}
	}

Mpeg2Decode_Exit:
	gst_buffer_unmap (pGstBuf,&mapInfo);

	FUNC_OUT();

	return ret;
}
//
//						End of the MPEG2 Decoder
//
//////////////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////////////
//
//							MPEG4 Decoder
//
static gint Mpeg4DecodeFrame( NX_VIDEO_DEC_STRUCT *pNxVideoDecHandle, GstBuffer *pGstBuf, NX_V4L2DEC_OUT *pDecOut )
{

	GstMapInfo mapInfo;
	guint8 *pInBuf = NULL;
	gint inSize = 0;
	guint8 *pDecBuf = NULL;
	gint decBufSize = 0;
	gint ret=0;
	NX_V4L2DEC_IN decIn;
	gint64 timestamp = 0;
	NX_VIDEO_DEC_STRUCT *pHDec = pNxVideoDecHandle;

	FUNC_IN();

	if( pHDec->bFlush )
	{
		FlushDecoder( pHDec );
		pHDec->bFlush = FALSE;
	}

	gst_buffer_map(pGstBuf, &mapInfo, GST_MAP_READ);
	pInBuf = mapInfo.data;
	inSize = gst_buffer_get_size(pGstBuf);

	// Push Input Time Stamp
	if ( GST_BUFFER_PTS_IS_VALID(pGstBuf) )
	{
		PushVideoTimeStamp(pHDec, GST_BUFFER_PTS(pGstBuf), GST_BUFFER_FLAGS(pGstBuf) );
		timestamp = GST_BUFFER_PTS(pGstBuf);
	}
	else if ( GST_BUFFER_DTS_IS_VALID(pGstBuf) )
	{
		PushVideoTimeStamp(pHDec, GST_BUFFER_DTS(pGstBuf), GST_BUFFER_FLAGS(pGstBuf) );
		timestamp = GST_BUFFER_DTS(pGstBuf);
	}

	if( FALSE == pHDec->bInitialized  )
	{
		pDecBuf = pHDec->pTmpStrmBuf;

		if( pHDec->extraDataSize  )
		{
			memcpy( pDecBuf, pHDec->pExtraData, pHDec->extraDataSize );
			decBufSize = pHDec->extraDataSize;
			memcpy( pDecBuf+decBufSize, pInBuf, inSize );
			decBufSize += inSize;
		}
		else
		{
			memcpy( pDecBuf, pInBuf, inSize );
			decBufSize = inSize;
		}

		// Initialize VPU
		ret = InitializeCodaVpu(pHDec, pDecBuf, decBufSize );

		if( 0 > ret )
		{
			g_print("VPU initialized Failed!!!!\n");
			NX_V4l2DecClose( pNxVideoDecHandle->hCodec );
			pNxVideoDecHandle->hCodec = NULL;
			goto Mpeg4Decode_Exit;

		}

		pHDec->bInitialized = TRUE;

		ret = 0;
		pDecOut->dispIdx = -1;
	}
	else
	{
		decIn.strmBuf = pInBuf;
		decIn.strmSize = inSize;
		decIn.timeStamp = timestamp;
		decIn.eos = 0;
		VDecSemPend(pHDec->pSem);
		ret = NX_V4l2DecDecodeFrame( pHDec->hCodec,&decIn, pDecOut );
		if( (0 != ret ) || (0 > pDecOut->dispIdx) )
		{
			VDecSemPost( pHDec->pSem );
		}
	}

Mpeg4Decode_Exit:
	gst_buffer_unmap (pGstBuf,&mapInfo);

	FUNC_OUT();

	return ret;

}
//
//						End of the MPEG4 Decoder
//
//////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////
static void InitVideoTimeStamp(NX_VIDEO_DEC_STRUCT *hDec)
{
	gint i;
	for( i=0 ;i<NX_MAX_BUF; i++ )
	{
		hDec->outTimeStamp[i].flag = (gint)-1;
		hDec->outTimeStamp[i].timestamp = (gint)0;
	}
	hDec->inFlag = 0;
	hDec->outFlag = 0;
}

static void PushVideoTimeStamp(NX_VIDEO_DEC_STRUCT *hDec, gint64 timestamp, guint flag )
{
	gint i=0;
	if(-1 != timestamp)
	{
		hDec->inFlag++;
		if(NX_MAX_BUF <= hDec->inFlag)
			hDec->inFlag = 0;

		for( i=0 ;i<NX_MAX_BUF; i++ )
		{
			if( hDec->outTimeStamp[i].flag == (gint)-1 )
			{
				hDec->outTimeStamp[i].timestamp = timestamp;
				hDec->outTimeStamp[i].flag = flag;
				break;
			}
		}
	}
}

static gint PopVideoTimeStamp(NX_VIDEO_DEC_STRUCT *hDec, gint64 *pTimestamp, guint *pFlag )
{
	gint i=0;
	gint64 minTime = 0x7FFFFFFFFFFFFFFFll;
	gint minIdx = -1;
	for( i=0 ;i<NX_MAX_BUF; i++ )
	{
		if( hDec->outTimeStamp[i].flag != (guint)-1 )
		{
			if( minTime > hDec->outTimeStamp[i].timestamp )
			{
				minTime = hDec->outTimeStamp[i].timestamp;
				minIdx = i;
			}
		}
	}
	if( minIdx != -1 )
	{
		*pTimestamp = hDec->outTimeStamp[minIdx].timestamp;
		*pFlag      = hDec->outTimeStamp[minIdx].flag;
		hDec->outTimeStamp[minIdx].flag = (gint)-1;
		return 0;
	}
	else
	{
		g_print("Cannot Found Time Stamp!!!\n");
		return -1;
	}
}
///////////////////////////////////////////////////////////////////////////////

//
//	Semaphore functions for output buffer.
//
NX_VDEC_SEMAPHORE *VDecSemCreate( int init )
{
	NX_VDEC_SEMAPHORE *pSem = (NX_VDEC_SEMAPHORE *)g_malloc(sizeof(NX_VDEC_SEMAPHORE));
	FUNC_IN();
	pSem->value = init;
	pthread_mutex_init(&pSem->mutex, NULL);
	pthread_cond_init(&pSem->cond, NULL);
	FUNC_OUT();
	return pSem;
}

void VDecSemDestroy( NX_VDEC_SEMAPHORE *pSem )
{
	FUNC_IN();
	if( pSem )
	{
		pthread_mutex_destroy( &pSem->mutex );
		pthread_cond_destroy( &pSem->cond );
		g_free( pSem );
	}
	FUNC_OUT();
}

gboolean VDecSemPend( NX_VDEC_SEMAPHORE *pSem )
{
	FUNC_IN();
	pthread_mutex_lock( &pSem->mutex );

	if( pSem->value == 0 ){
		pthread_cond_wait( &pSem->cond, &pSem->mutex );
	}
	pSem->value --;

	pthread_mutex_unlock( &pSem->mutex );
	FUNC_OUT();
	return TRUE;
}

gboolean VDecSemPost( NX_VDEC_SEMAPHORE *pSem )
{
	FUNC_IN();
	pthread_mutex_lock( &pSem->mutex );

	pSem->value ++;
	pthread_cond_signal( &pSem->cond );

	pthread_mutex_unlock( &pSem->mutex );
	FUNC_OUT();
	return TRUE;
}

gboolean VDecSemSignal( NX_VDEC_SEMAPHORE *pSem )
{
	FUNC_IN();
	pthread_mutex_lock( &pSem->mutex );
	pthread_cond_signal( &pSem->cond );
	pthread_mutex_unlock( &pSem->mutex );
	FUNC_OUT();
	return TRUE;
}
