/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "refreshmap.h"
#include "procevents.h"

#include <cstdio>
#include <cstdlib>







// Aligned so the first entry starts on line 30.
int refreshmap[] = {
    ALL,              // EvPhotoLoaded,
    0,              // EvProfileLoaded : obsolete,
    ALL,              // EvProfileChanged,
    ALL,              // EvHistoryBrowsed,
    TONE,            // EvBrightness,
    TONE,            // EvContrast,
    TONE,            // EvBlack,
    TONE,            // EvExpComp,
    TONE,            // EvHLCompr,
    TONE,            // EvSHCompr,
    TONE,            // EvToneCurve1,
    AUTOEXP,          // EvAutoExp,
    AUTOEXP,          // EvClip,
    LUMINANCECURVE,   // EvLBrightness,
    LUMINANCECURVE,   // EvLContrast,
    0,   // EvLBlack : obsolete,
    0,   // EvLHLCompr : obsolete,
    0,   // EvLSHCompr : obsolete,
    LUMINANCECURVE,   // EvLLCurve,
    SHARPENING,       // EvShrEnabled,
    SHARPENING,       // EvShrRadius,
    SHARPENING,       // EvShrAmount,
    SHARPENING,       // EvShrThresh,
    SHARPENING,       // EvShrEdgeOnly,
    SHARPENING,       // EvShrEdgeRadius,
    SHARPENING,       // EvShrEdgeTolerance,
    SHARPENING,       // EvShrHaloControl,
    SHARPENING,       // EvShrHaloAmount,
    SHARPENING,       // EvShrMethod,
    SHARPENING,       // EvShrDRadius,
    SHARPENING,       // EvShrDAmount,
    SHARPENING,       // EvShrDDamping,
    SHARPENING,       // EvShrDIterations,
    HDR,        // EvLCPUseDist,
    DARKFRAME,        // EvLCPUseVign,
    HDR,        // EvLCPUseCA,
    M_VOID,           // EvFixedExp
    WB,               // EvWBMethod,
    WB,               // EvWBTemp,
    WB,               // EvWBGreen,
    TONE,            // EvToneCurveMode1,
    TONE,            // EvToneCurve2,
    TONE,            // EvToneCurveMode2,
    0,                // EvLDNRadius: obsolete,
    0,                // EvLDNEdgeTolerance: obsolete,
    0,                // EvCDNEnabled:obsolete,
    0,                // free entry
    ALLNORAW, //RGBCURVE | M_AUTOEXP, // EvDCPToneCurve, 21 july 2024
    ALLNORAW,         // EvDCPIlluminant,
    LUMINANCECURVE,          // EvSHEnabled,
    LUMINANCECURVE,         // EvSHHighlights,
    LUMINANCECURVE,         // EvSHShadows,
    LUMINANCECURVE,         // EvSHHLTonalW,
    LUMINANCECURVE,         // EvSHSHTonalW,
    0,         // EvSHLContrast : obsolete,
    LUMINANCECURVE,          // EvSHRadius,
    ALLNORAW,         // EvCTRotate,
    ALLNORAW,         // EvCTHFlip,
    ALLNORAW,         // EvCTVFlip,
    TRANSFORM,  // EvROTDegree,
    HDR,        // EvTransAutoFill,
    HDR,        // EvDISTAmount,
    ALL,              // EvBookmarkSelected,
    CROP,             // EvCrop,
    HDR,        // EvCACorr,
    ALLNORAW,         // EvHREnabled,
    0,         // EvHRAmount : obsolete,
    ALLNORAW|M_RAW,   // EvHRMethod,
    DEMOSAIC,         // EvWProfile,
    OUTPUTPROFILE,    // EvOProfile,
    ALLNORAW,         // EvIProfile,
    HDR,        // EvVignettingAmount,
    TONE,            // EvChMixer,
    RESIZE,           // EvResizeScale,
    RESIZE,           // EvResizeMethod,
    EXIF,             // EvExif,
    IPTC,             // EvIPTC
    0,           // EvResizeSpec : obsolete,
    RESIZE,           // EvResizeWidth
    RESIZE,           // EvResizeHeight
    RESIZE,           // EvResizeEnabled
    ALL,              // EvProfileChangeNotification
    0,          // EvSHHighQuality : obsolete
    HDR,        // EvPerspCorr
    DARKFRAME,        // EvLCPFile
    TONE,            // EvRGBrCurveLumamode
    IMPULSEDENOISE,   // EvIDNEnabled,
    IMPULSEDENOISE,   // EvIDNThresh,
    ALLNORAW,         // EvDPDNEnabled,
    ALLNORAW,         // EvDPDNLuma,
    ALLNORAW,         // EvDPDNChroma,
    ALLNORAW,         // EvDPDNGamma,
    RGBCURVE,         // EvDirPyrEqualizer,
    RGBCURVE,         // EvDirPyrEqlEnabled,
    LUMINANCECURVE,   // EvLSaturation,
    LUMINANCECURVE,   // EvLaCurve,
    LUMINANCECURVE,   // EvLbCurve,
    DEMOSAIC,         // EvDemosaicMethod
    DARKFRAME,        // EvPreProcessHotPixel
    TONE,            // EvSaturation,
    TONE,            // EvHSVEqualizerH,
    TONE,            // EvHSVEqualizerS,
    TONE,            // EvHSVEqualizerV,
    TONE,            // EvHSVEqEnabled,
    DEFRINGE,         // EvDefringeEnabled,
    DEFRINGE,         // EvDefringeRadius,
    DEFRINGE,         // EvDefringeThreshold,
    TONE,            // EvHLComprThreshold,
    RESIZE,           // EvResizeBoundingBox
    RESIZE,           // EvResizeAppliesTo
    LUMINANCECURVE,   // EvCBAvoidClip,
    LUMINANCECURVE,   // EvCBSatLimiter,
    LUMINANCECURVE,   // EvCBSatLimit,
    DEMOSAIC,         // EvDemosaicDCBIter
    ALLNORAW,         // EvDemosaicFalseColorIter
    DEMOSAIC,         // EvDemosaicDCBEnhanced
    DARKFRAME,        // EvPreProcessCARed
    DARKFRAME,        // EvPreProcessCABlue
    DARKFRAME,        // EvPreProcessLineDenoise
    DARKFRAME,        // EvPreProcessGEquilThresh
    DARKFRAME,        // EvPreProcessAutoCA
    DARKFRAME,        // EvPreProcessAutoDF
    DARKFRAME,        // EvPreProcessDFFile
    DARKFRAME,        // EvPreProcessExpCorrLinear
    0,                // --unused--
    FLATFIELD,        // EvFlatFieldFile,
    FLATFIELD,        // EvFlatFieldAutoSelect,
    FLATFIELD,        // EvFlatFieldBlurRadius,
    FLATFIELD,        // EvFlatFieldBlurType,
    HDR,        // EvAutoDIST,
    0,         // EvDPDNLumCurve : obsolete
    0,         // EvDPDNChromCurve : obsolete
    0,            // EvGAMMA : obsolete
    0,            // EvGAMPOS : obsolete
    0,            // EvGAMFREE : obsolete
    0,            // EvSLPOS : obsolete
    DARKFRAME,        // EvPreProcessExpBlackzero
    DARKFRAME,        // EvPreProcessExpBlackone
    DARKFRAME,        // EvPreProcessExpBlacktwo
    DARKFRAME,        // EvPreProcessExpBlackthree
    DARKFRAME,        // EvPreProcessExptwoGreen
    SHARPENING,       // EvSharpenEdgePasses
    SHARPENING,       // EvSharpenEdgeStrength
    SHARPENING,       // EvSharpenMicroStrength
    SHARPENING,       // EvSharpenMicroUniformity
    SHARPENING,       // EvSharpenEdgeEnabled
    SHARPENING,       // EvSharpenEdgeThreechannels
    SHARPENING,       // EvSharpenMicroEnabled
    SHARPENING,       // EvSharpenMicroMatrix
    DEMOSAIC,         // EvDemosaicALLEnhanced Disabled but not removed for now, may be reintroduced some day
    TONE,            // EvVibranceEnabled
    TONE,            // EvVibrancePastels
    TONE,            // EvVibranceSaturated
    TONE,            // EvVibranceProtectSkins
    TONE,            // EvVibranceAvoidColorShift
    TONE,            // EvVibrancePastSatTog
    TONE,            // EvVibrancePastSatThreshold
    SHARPENING,       // EvEPDStrength
    SHARPENING,       // EvEPDEdgeStopping
    SHARPENING,       // EvEPDScale
    SHARPENING,       // EvEPDReweightingIterates
    SHARPENING,       // EvEPDEnabled
    TONE,            // EvRGBrCurve
    TONE,            // EvRGBgCurve
    TONE,            // EvRGBbCurve
    AUTOEXP,         // EvNeutralExp
    DEMOSAIC | M_PREPROC, // EvDemosaicMethodPreProc
    LUMINANCECURVE,   // EvLCCurve
    LUMINANCECURVE,   // EvLCHCurve
    TONE,            // EvVibranceSkinTonesCurve
    LUMINANCECURVE,   // EvLLCCurve
    LUMINANCECURVE,   // EvLLCredsk
    ALLNORAW,         // EvDPDNLdetail
    LUMINANCECURVE,         // EvCATEnabled
    LUMINANCECURVE,   // EvCATDegree
    LUMINANCECURVE,   // EvCATMethodsur
    LUMINANCECURVE,   // EvCATAdapscen
    LUMINANCECURVE,   // EvCATAdapLum
    LUMINANCECURVE,   // EvCATMethodWB
    LUMINANCECURVE,   // EvCATJLight
    LUMINANCECURVE,   // EvCATChroma
    LUMINANCECURVE,   // EvCATAutoDegree
    LUMINANCECURVE,   // EvCATContrast
    LUMINANCECURVE,   // EvCATSurr
    LUMINANCECURVE,   // EvCATgamut
    LUMINANCECURVE,   // EvCATmethodalg
    LUMINANCECURVE,   // EvCATRstpro
    LUMINANCECURVE,   // EvCATQbright
    LUMINANCECURVE,   // EvCATQContrast
    LUMINANCECURVE,   // EvCATSChroma
    LUMINANCECURVE,   // EvCATMchroma
    LUMINANCECURVE,   // EvCAThue
    LUMINANCECURVE,   // EvCATcurve1
    LUMINANCECURVE,   // EvCATcurve2
    LUMINANCECURVE,   // EvCATcurvemode1
    LUMINANCECURVE,   // EvCATcurvemode2
    LUMINANCECURVE,   // EvCATcurve3
    LUMINANCECURVE,   // EvCATcurvemode3
    LUMINANCECURVE,   // EvCATdatacie
    LUMINANCECURVE,   // EvCATtonecie
    ALLNORAW,         // EvDPDNbluechro
    ALLNORAW,         // EvDPDNperform
    ALLNORAW,         // EvDPDNmet
    DEMOSAIC,         // EvDemosaicLMMSEIter
    LUMINANCECURVE,   // EvCATbadpix
    LUMINANCECURVE,   // EvCATAutoadap
    DEFRINGE,         // EvPFCurve
    WB,               // EvWBequal
    0,         // EvWBequalbo : obsolete
    GEOMETRY,   // EvGradientDegree
    GEOMETRY,   // EvGradientEnabled
    GEOMETRY,   // EvPCVignetteStrength
    GEOMETRY,   // EvPCVignetteEnabled
    TONE,            // EvBWChmixEnabled
    TONE,            // EvBWred
    TONE,            // EvBWgreen
    TONE,            // EvBWblue
    TONE,            // EvBWredgam
    TONE,            // EvBWgreengam
    TONE,            // EvBWbluegam
    TONE,            // EvBWfilter
    TONE,            // EvBWsetting
    TONE,            // EvBWoran
    TONE,            // EvBWyell
    TONE,            // EvBWcyan
    TONE,            // EvBWmag
    TONE,            // EvBpur
    TONE,            // EvBWLuminanceEqual
    TONE,            // EvBWChmixEnabledLm
    TONE,            // EvBWmethod
    TONE,            // EvBWBeforeCurve
    TONE,            // EvBWBeforeCurveMode
    TONE,            // EvBWAfterCurve
    TONE,            // EvBWAfterCurveMode
    TONE,            // EvAutoch
    0,                // --unused--
    TONE,            // EvNeutralBW
    GEOMETRY,   // EvGradientFeather
    GEOMETRY,   // EvGradientStrength
    GEOMETRY,   // EvGradientCenter
    GEOMETRY,   // EvPCVignetteFeather
    GEOMETRY,   // EvPCVignetteRoundness
    GEOMETRY,   // EvVignettingRadius,
    GEOMETRY,   // EvVignettingStrength
    GEOMETRY,   // EvVignettingCenter
    LUMINANCECURVE,   // EvLCLCurve
    LUMINANCECURVE,   // EvLLHCurve
    LUMINANCECURVE,   // EvLHHCurve
    RGBCURVE,         // EvDirPyrEqualizerThreshold
    0,         // EvDPDNenhance : obsolete
    TONE,            // EvBWMethodalg
    RGBCURVE,         // EvDirPyrEqualizerSkin
    RGBCURVE,         // EvDirPyrEqlgamutlab
    RGBCURVE,         // EvDirPyrEqualizerHueskin
    ALLNORAW,         // EvDPDNmedian
    ALLNORAW,         // EvDPDNmedmet
    TONE,            // EvColorToningEnabled
    TONE,            // EvColorToningColor
    TONE,            // EvColorToningOpacity
    TONE,            // EvColorToningCLCurve
    TONE,            // EvColorToningMethod
    TONE,            // EvColorToningLLCurve
    TONE,            // EvColorToningredlow
    TONE,            // EvColorToninggreenlow
    TONE,            // EvColorToningbluelow
    TONE,            // EvColorToningredmed
    TONE,            // EvColorToninggreenmed
    TONE,            // EvColorToningbluemed
    TONE,            // EvColorToningredhigh
    TONE,            // EvColorToninggreenhigh
    TONE,            // EvColorToningbluehigh
    TONE,            // EvColorToningbalance
    TONE,            // EvColorToningNeutral
    0,         // EvColorToningsatlow : obsolete
    0,         // EvColorToningsathigh : obsolete
    TONE,            // EvColorToningTwocolor
    TONE,            // EvColorToningNeutralcur
    TONE,            // EvColorToningLumamode
    TONE,            // EvColorToningShadows
    TONE,            // EvColorToningHighights
    TONE,            // EvColorToningSatProtection
    TONE,            // EvColorToningSatThreshold
    TONE,            // EvColorToningStrength
    TONE,            // EvColorToningautosat
    ALLNORAW,         // EvDPDNmetmed
    ALLNORAW,         // EvDPDNrgbmet
    ALLNORAW,         // EvDPDNpasses
    FLATFIELD,        // EvFlatFieldClipControl
    FLATFIELD,        // EvFlatFieldAutoClipControl
    DARKFRAME,        // EvPreProcessExpBlackRed
    DARKFRAME,        // EvPreProcessExpBlackGreen
    DARKFRAME,        // EvPreProcessExpBlackBlue
    TONE,            // EvFilmSimulationEnabled (the HaldCLUT is applied in rgbProc)
    TONE,            // EvFilmSimulationStrength
    TONE,            // EvFilmSimulationFilename
    ALLNORAW,         // EvDPDNLCurve
    ALLNORAW,         // EvDPDNsmet
    DARKFRAME,        // EvPreProcessDeadPixel
    ALLNORAW,         // EvDPDNCCCurve
    0,         // EvDPDNautochroma : obsolete
    ALLNORAW,         // EvDPDNLmet
    ALLNORAW,         // EvDPDNCmet
    ALLNORAW,         // EvDPDNC2met
    DIRPYREQUALIZER,  // EvWavelet
    DIRPYREQUALIZER,  // EvEnabled
    DIRPYREQUALIZER,  // EvWavLmethod
    DIRPYREQUALIZER,  // EvWavCLmethod
    DIRPYREQUALIZER,  // EvWavDirmethod
    DIRPYREQUALIZER,  // EvWavtiles
    DIRPYREQUALIZER,  // EvWavsky
    DIRPYREQUALIZER,  // EvWavthres
    DIRPYREQUALIZER,  // EvWavthr
    DIRPYREQUALIZER,  // EvWavchroma
    DIRPYREQUALIZER,  // EvWavmedian
    DIRPYREQUALIZER,  // EvWavunif
    DIRPYREQUALIZER,  // EvWavSkin
    DIRPYREQUALIZER,  // EvWavHueSkin
    DIRPYREQUALIZER,  // EvWavThreshold
    DIRPYREQUALIZER,  // EvWavlhl
    DIRPYREQUALIZER,  // EvWavbhl
    DIRPYREQUALIZER,  // EvWavThresHold2
    DIRPYREQUALIZER,  // EvWavavoid
    DIRPYREQUALIZER,  // EvWavCCCurve
    DIRPYREQUALIZER,  // EvWavpast
    DIRPYREQUALIZER,  // EvWavsat
    DIRPYREQUALIZER,  // EvWavCHmet
    DIRPYREQUALIZER,  // EvWavHSmet
    DIRPYREQUALIZER,  // EvWavchro
    DIRPYREQUALIZER,  // EvWavColor
    DIRPYREQUALIZER,  // EvWavOpac
    DIRPYREQUALIZER,  // EvWavsup
    DIRPYREQUALIZER,  // EvWavTilesmet
    DIRPYREQUALIZER,  // EvWavrescon
    DIRPYREQUALIZER,  // EvWavreschro
    DIRPYREQUALIZER,  // EvWavresconH
    DIRPYREQUALIZER,  // EvWavthrH
    DIRPYREQUALIZER,  // EvWavHueskin2
    DIRPYREQUALIZER,  // EvWavedgrad
    DIRPYREQUALIZER,  // EvWavedgval
    DIRPYREQUALIZER,  // EvWavStrngth
    DIRPYREQUALIZER,  // EvWavdaubcoeffmet
    DIRPYREQUALIZER,  // EvWavedgreinf
    DIRPYREQUALIZER,  // EvWaveletch
    DIRPYREQUALIZER,  // EvWavCHSLmet
    DIRPYREQUALIZER,  // EvWavedgcont
    DIRPYREQUALIZER,  // EvWavEDmet
    DIRPYREQUALIZER,  // EvWavlev0nois
    DIRPYREQUALIZER,  // EvWavlev1nois
    DIRPYREQUALIZER,  // EvWavlev2nois
    DIRPYREQUALIZER,  // EvWavmedianlev
    DIRPYREQUALIZER,  // EvWavHHCurve
    DIRPYREQUALIZER,  // EvWavBackmet
    DIRPYREQUALIZER,  // EvWavedgedetect
    DIRPYREQUALIZER,  // EvWavlipst
    DIRPYREQUALIZER,  // EvWavedgedetectthr
    DIRPYREQUALIZER,  // EvWavedgedetectthr2
    DIRPYREQUALIZER,  // EvWavlinkedg
    DIRPYREQUALIZER,  // EvWavCHCurve
    DARKFRAME,        // EvPreProcessHotDeadThresh
    SHARPENING,       // EvEPDgamma
    DIRPYREQUALIZER,  // EvWavtmr
    DIRPYREQUALIZER,  // EvWavTMmet
    DIRPYREQUALIZER,  // EvWavtmrs
    DIRPYREQUALIZER,  // EvWavbalance
    DIRPYREQUALIZER,  // EvWaviter
    DIRPYREQUALIZER,  // EvWavgamma
    DIRPYREQUALIZER,  // EvWavCLCurve
    DIRPYREQUALIZER,  // EvWavopacity
    DIRPYREQUALIZER,  // EvWavBAmet
    DIRPYREQUALIZER,  // EvWavopacityWL
    RESIZE,           // EvPrShrEnabled
    RESIZE,           // EvPrShrRadius
    RESIZE,           // EvPrShrAmount
    RESIZE,           // EvPrShrThresh
    RESIZE,           // EvPrShrEdgeOnly
    RESIZE,           // EvPrShrEdgeRadius=375,
    RESIZE,           // EvPrShrEdgeTolerance=376,
    RESIZE,           // EvPrShrHaloControl=377,
    RESIZE,           // EvPrShrHaloAmount=378,
    RESIZE,           // EvPrShrMethod=379,
    RESIZE,           // EvPrShrDRadius=380,
    RESIZE,           // EvPrShrDAmount=381,
    RESIZE,           // EvPrShrDDamping=382,
    RESIZE,           // EvPrShrDIterations=383,
    DIRPYREQUALIZER,  // EvWavcbenab
    DIRPYREQUALIZER,  // EvWavgreenhigh
    DIRPYREQUALIZER,  // EvWavbluehigh
    DIRPYREQUALIZER,  // EvWavgreenmed
    DIRPYREQUALIZER,  // EvWavbluemed
    DIRPYREQUALIZER,  // EvWavgreenlow
    DIRPYREQUALIZER,  // EvWavbluelow
    DIRPYREQUALIZER,  // EvWavNeutral
    ALLNORAW, //RGBCURVE | M_AUTOEXP, // EvDCPApplyLookTable,  21 july 2024
    ALLNORAW, //RGBCURVE | M_AUTOEXP, // EvDCPApplyBaselineExposureOffset, 21 july 2024
    ALLNORAW,         // EvDCPApplyHueSatMap
    DIRPYREQUALIZER,  // EvWavenacont
    DIRPYREQUALIZER,  // EvWavenachrom
    DIRPYREQUALIZER,  // EvWavenaedge
    DIRPYREQUALIZER,  // EvWavenares
    DIRPYREQUALIZER,  // EvWavenafin
    DIRPYREQUALIZER,  // EvWavenatoning
    DIRPYREQUALIZER,  // EvWavenanoise
    DIRPYREQUALIZER,  // EvWavedgesensi
    DIRPYREQUALIZER,  // EvWavedgeampli
    DIRPYREQUALIZER,  // EvWavlev3nois
    DIRPYREQUALIZER,  // EvWavNPmet
    DEMOSAIC,         // EvretinexMethod
    RETINEX,          // EvLneigh
    0,          // EvLgain : obsolete
    RETINEX,          // EvLoffs
    RETINEX,          // EvLstr
    RETINEX,          // EvLscal
    RETINEX,          // EvLvart
    DEMOSAIC,         // EvLCDCurve
    RETINEX,          // EvRetinextransmission
    DEMOSAIC,         // EvRetinexEnabled
    RETINEX,          // EvRetinexmedianmap
    RETINEX,          // EvLlimd
    DEMOSAIC,         // Evretinexcolorspace
    DEMOSAIC,         // EvLCDHCurve
    DEMOSAIC,         // Evretinexgamma
    DEMOSAIC,         // EvLgam
    DEMOSAIC,         // EvLslope
    RETINEX,          // EvLhighl
    0,                // --unused--
    DEMOSAIC,         // EvRetinexlhcurve
    OUTPUTPROFILE,    // EvOIntent
    MONITORTRANSFORM, // EvMonitorTransform: no history message
    RETINEX,          // EvLiter
    RETINEX,          // EvLgrad
    RETINEX,          // EvLgrads
    RETINEX,          // EvLhighlights
    RETINEX,          // EvLh_tonalwidth
    RETINEX,          // EvLshadows
    RETINEX,          // EvLs_tonalwidth
    RETINEX,          // EvLradius
    RETINEX,          // EvmapMethod
    DEMOSAIC,         // EvRetinexmapcurve
    DEMOSAIC,         // EvviewMethod
    RGBCURVE,         // EvcbdlMethod
    RETINEX,          // EvRetinexgaintransmission
    RETINEX,          // EvLskal
    OUTPUTPROFILE,    // EvOBPCompens
    WB,               // EvWBtempBias
    DARKFRAME,        // EvRawImageNum
    0,                // unused
    0,                // unused
    0,                // unused
    DEMOSAIC,         // EvPixelShiftEperIso
    0,                // unused
    0,                // unused
    DEMOSAIC,         // EvPixelshiftShowMotion
    DEMOSAIC,         // EvPixelshiftShowMotionMaskOnly
    0,                // unused
    0,                // unused
    0,                // unused
    DEMOSAIC,         // EvPixelShiftNonGreenCross
    0,                // unused
    0,                // unused
    0,                // unused
    0,                // unused
    DEMOSAIC,         // EvPixelShiftGreen
    0,                // unused
    DEMOSAIC,         // EvPixelShiftBlur
    DEMOSAIC,         // EvPixelShiftSigma
    0,                // unused
    0,                // unused
    DEMOSAIC,         // EvPixelShiftHoleFill
    DEMOSAIC,         // EvPixelShiftMedian
    0,                // unused
    DEMOSAIC,         // EvPixelShiftMotionMethod
    DEMOSAIC,         // EvPixelShiftSmooth
    0,         // EvPixelShiftLmmse : obsolete
    DEMOSAIC,         // EvPixelShiftEqualBright
    DEMOSAIC,         // EvPixelShiftEqualBrightChannel
    LUMINANCECURVE,   // EvCATtempout
    LUMINANCECURVE,   // EvCATgreenout
    LUMINANCECURVE,   // EvCATybout
    LUMINANCECURVE,   // EvCATDegreeout
    LUMINANCECURVE,   // EvCATAutoDegreeout
    LUMINANCECURVE,   // EvCATtempsc
    LUMINANCECURVE,   // EvCATgreensc
    LUMINANCECURVE,   // EvCATybscen
    LUMINANCECURVE,   // EvCATAutoyb
    DARKFRAME,        // EvLensCorrMode
    DARKFRAME,        // EvLensCorrLensfunCamera
    DARKFRAME,        // EvLensCorrLensfunLens
    ALLNORAW,         // EvTMFattalEnabled
    HDR,              // EvTMFattalThreshold
    HDR,              // EvTMFattalAmount
    WB,               // EvWBEnabled
    TONE,            // EvRGBEnabled
    LUMINANCECURVE,   // EvLEnabled
    DEMOSAIC,         // EvPdShrEnabled
    CAPTURESHARPEN,    // EvPdShrMaskToggled
    TONE,      // EvLocallabSpotDeleted
    TONE,           // EvLocallabSpotSelected
    M_VOID,           // EvLocallabSpotName
    M_VOID,           // EvLocallabSpotVisibility
    TONE,      // EvLocallabSpotShape
    TONE,      // EvLocallabSpotSpotMethod
    TONE,      // EvLocallabSpotShapeMethod
    TONE,      // EvLocallabSpotLocX
    TONE,      // EvLocallabSpotLocXL
    TONE,      // EvLocallabSpotLocY
    TONE,      // EvLocallabSpotLocYT
    TONE,      // EvLocallabSpotCenter
    TONE,      // EvLocallabSpotCircrad
    TONE,      // EvLocallabSpotQualityMethod
    TONE,      // EvLocallabSpotTransit
    TONE,      // EvLocallabSpotThresh
    TONE,      // EvLocallabSpotIter
    TONE,      // EvLocallabSpotSensiexclu
    TONE,      // EvLocallabSpotStruc
    TONE,      // EvlocallabEnabled
    TONE,      // EvLocenacolor
    TONE,      // Evlocallabcurvactiv
    TONE,      // Evlocallablightness
    TONE,      // Evlocallabcontrast
    TONE,      // Evlocallabchroma
    TONE,      // Evlocallabsensi
    TONE,      // EvlocallabqualitycurveMethod
    TONE,      // Evlocallabllshape
    TONE,      // Evlocallabccshape
    TONE,      // EvlocallabLHshape
    TONE,      // EvlocallabHHshape
    TONE,      // Evlocallabinvers
    TONE,      // EvLocenaexpose
    TONE,      // Evlocallabexpcomp
    TONE,      // Evlocallabhlcompr
    TONE,      // Evlocallabhlcomprthresh
    TONE,      // Evlocallabblack
    TONE,      // Evlocallabshcompr
    TONE,      // Evlocallabwarm
    TONE,      // Evlocallabsensiex
    TONE,      // Evlocallabshapeexpos
    TONE,      // EvLocenavibrance
    TONE,      // EvlocallabSaturated
    TONE,      // EvlocallabPastels
    TONE,      // EvlocallabPastSatThreshold
    TONE,      // EvlocallabProtectSkins
    TONE,      // EvlocallabAvoidColorShift
    TONE,      // EvlocallabPastSatTog
    TONE,      // Evlocallabsensiv
    TONE,      // EvlocallabSkinTonesCurve
    TONE,      // EvLocenablur
    TONE,      // Evlocallabradius
    TONE,      // Evlocallabstrength
    TONE,      // Evlocallabsensibn
    TONE,      // EvlocallabblurMethod
    TONE,      // Evlocallabactivlum
    TONE,      // EvLocenatonemap
    TONE,      // Evlocallabstren
    TONE,      // Evlocallabgamma
    TONE,      // Evlocallabestop
    TONE,      // Evlocallabscaltm
    TONE,      // Evlocallabrewei
    TONE,      // Evlocallabsensitm
    TONE,      // EvLocenareti
    TONE,      // EvlocallabretinexMethod
    TONE,      // Evlocallabstr
    TONE,      // Evlocallabchrrt
    TONE,      // Evlocallabneigh
    TONE,      // Evlocallabvart
    TONE,      // Evlocallabsensih
    TONE,      // EvlocallabCTgainCurve
    TONE,      // Evlocallabinversret
    TONE,      // EvLocenasharp
    TONE,      // Evlocallabsharradius
    TONE,      // Evlocallabsharamount
    TONE,      // Evlocallabshardamping
    TONE,      // Evlocallabshariter
    TONE,      // Evlocallabsensis
    TONE,      // Evlocallabinverssha
    TONE,      // EvLocenacbdl
    TONE,      // EvlocallabEqualizer
    TONE,      // Evlocallabchromacbdl
    TONE,      // EvlocallabThresho
    TONE,      // Evlocallabsensicb
    TONE,      // EvLocenadenoi
    TONE,      // Evlocallabnoiselumf
    TONE,      // Evlocallabnoiselumc
    TONE,      // Evlocallabnoiselumdetail
    TONE,      // Evlocallabnoiselequal
    TONE,      // Evlocallabnoisechrof
    TONE,      // Evlocallabnoisechroc
    TONE,      // Evlocallabnoisechrodetail
    TONE,      // Evlocallabadjblur
    TONE,      // Evlocallabbilateral
    TONE,      // Evlocallabsensiden
    TONE,      // Evlocallabavoid
    TONE,      // Evlocallabsharcontrast
    TONE,      // EvLocenacontrast
    TONE,      // Evlocallablcradius
    TONE,      // Evlocallablcamount
    TONE,      // Evlocallablcdarkness
    TONE,      // Evlocallablclightness
    TONE,      // Evlocallabsensilc
    TONE,      // Evlocallabdehaz
    TONE,      // EvLocenasoft
    TONE,      // EvLocallabstreng
    TONE,      // EvLocallabsensisf
    TONE,      // Evlocallabsharblur
    0,   // EvLocenalabregion : obsolete
    TONE,      // EvlocallabshowmaskMethod
    TONE,      // EvLocallabSpotSelectedWithMask
    TONE,      // EvlocallabCCmaskshape
    TONE,      // EvlocallabLLmaskshape
    TONE,      // EvlocallabCCmaskexpshape
    TONE,      // EvlocallabLLmaskexpshape
    TONE,      // EvlocallabHHmaskshape
    TONE,      // Evlocallabstructcol
    TONE,      // Evlocallabstructexp
    TONE,      // EvlocallabHHmaskexpshape
    TONE,      // Evlocallabblendmaskcol
    TONE,      // Evlocallabblendmaskexp
    TONE,      // Evlocallabblurexpde
    TONE,      // EvLocallabEnaColorMask
    TONE,      // EvLocallabEnaExpMask
    TONE,      // Evlocallabblurcolde
    TONE,      // Evlocallabinversex
    TONE,      // Evlocallabstructexclu
    TONE,      // Evlocallabexpchroma
    TONE,      // EvLocallabLabGridValue
    TONE,      // EvLocallabLabstrengthgrid
    TONE,      // EvLocallabgridMethod
    TONE,      // EvLocenashadhigh
    TONE,      // EvLocallabhighlights
    TONE,      // EvLocallabh_tonalwidth
    TONE,      // EvLocallabshadows
    TONE,      // EvLocallabs_tonalwidth
    TONE,      // EvLocallabsh_radius
    TONE,      // EvLocallabsensihs
    TONE,      // Evlocallabradmaskcol
    TONE,      // Evlocallabradmaskexp
    TONE,      // EvlocallabToolAdded
    TONE,      // EvlocallabCCmaskSHshape
    TONE,      // EvlocallabLLmaskSHshape
    TONE,      // EvlocallabHHmaskSHshape
    TONE,      // EvlocallabblendmaskSH
    TONE,      // EvLocallabEnaSHMask
    TONE,      // EvlocallabradmaskSH
    TONE,      // EvlocallabblurSHde
    TONE,      // Evlocallabinverssh
    TONE,      // EvLocallabSpotbalan
    TONE,      // EvLocallabchromaskexp
    TONE,      // EvLocallabgammaskexp
    TONE,      // EvLocallabslomaskexp
    TONE,      // EvLocallabsoftradiusexp
    TONE,      // EvLocallabchromaskcol
    TONE,      // EvLocallabgammaskcol
    TONE,      // EvLocallabslomaskcol
    TONE,      // EvLocallabchromaskSH
    TONE,      // EvLocallabgammaskSH
    TONE,      // EvLocallabslomaskSH
    TONE,      // EvLocallabsoftradiuscol
    TONE,      // EvLocallabsoftradiusret
    TONE,      // EvLocallabsoftradiuscb
    TONE,      // EvLocallabSpotTransitweak
    TONE,      // EvLocallabclarityml
    TONE,      // EvLocallabcontresid
    TONE,      // Evlocallabnoiselumf0
    TONE,      // Evlocallabnoiselumf2
    0,   // Evlocallabblurcbdl
    TONE,      // Evlocallabblendmaskcb
    TONE,      // Evlocallabradmaskcb
    TONE,      // Evlocallabchromaskcb
    TONE,      // Evlocallabgammaskcb
    TONE,      // Evlocallabslomaskcb
    TONE,      // EvlocallabCCmaskcbshape
    TONE,      // EvlocallabLLmaskcbshape
    TONE,      // EvlocallabHHmaskcbshape
    TONE,      // EvLocallabEnacbMask
    M_VOID,           // EvlocallabToolRemovedWithoutRefresh
    TONE,      // Evlocallabsoftradiustm
    TONE,      // EvLocallabSpotTransitgrad
    TONE,      // Evlocallabamount
    TONE,      // Evlocallabsatur
    TONE,      // EvlocallabCCmaskretishape
    TONE,      // EvlocallabLLmaskretishape
    TONE,      // EvlocallabHHmaskretishape
    TONE,      // EvLocallabEnaretiMask
    TONE,      // Evlocallabblendmaskreti
    TONE,      // Evlocallabradmaskreti
    TONE,      // Evlocallabchromaskreti
    TONE,      // Evlocallabgammaskreti
    TONE,      // Evlocallabslomaskreti
    TONE,      // EvlocallabToolRemovedWithRefresh
    TONE,      // EvLocallabEnaretiMasktmap
    TONE,      // Evlocallabscalereti
    TONE,      // Evlocallabdarkness
    TONE,      // Evlocallablightnessreti
    TONE,      // Evlocallablimd
    TONE,      // Evlocallablaplace
    TONE,      // EvlocallabsoftMethod
    TONE,      // Evlocallabequilret
    TONE,      // Evlocallabequiltm
    TONE,      // Evlocallabfftwlc
    TONE,      // Evlocallabfftwreti
    TONE,      // EvlocallabshowmasksoftMethod
    TONE,      // Evlocallabshadex
    TONE,      // EvlocallabexpMethod
    TONE,      // EvLocallablaplacexp
    TONE,      // EvLocallabbalanexp
    TONE,      // EvLocallablinear
    TONE,      // EvlocallabCCmasktmshape
    TONE,      // EvlocallabLLmasktmshape
    TONE,      // EvlocallabHHmasktmshape
    TONE,      // EvLocallabEnatmMask
    TONE,      // Evlocallabblendmasktm
    TONE,      // Evlocallabradmasktm
    TONE,      // Evlocallabchromasktm
    TONE,      // Evlocallabgammasktm
    TONE,      // Evlocallabslomasktm
    TONE,      // EvlocallabshowmasktmMethod
    TONE,      // EvlocallablocalcontMethod
    TONE,      // Evlocallabwavcurve
    TONE,      // Evlocallablevelwav
    TONE,      // Evlocallabresidcont
    TONE,      // EvlocallabCCmaskblshape
    TONE,      // EvlocallabLLmaskblshape
    TONE,      // EvlocallabHHmaskblshape
    TONE,      // EvLocallabEnablMask
    TONE,      // EvlocallabshowmaskblMethod
    TONE,      // Evlocallabblendmaskbl
    TONE,      // Evlocallabradmaskbl
    TONE,      // Evlocallabchromaskbl
    TONE,      // Evlocallabgammaskbl
    TONE,      // Evlocallabslomaskbl
    TONE,      // EvlocallabblMethod
    TONE,      // EvlocallabmedMethod
    TONE,      // Evlocallabitera
    TONE,      // Evlocallabguidbl
    TONE,      // Evlocallabepsbl
    TONE,      // EvlocallabshowmaskcolMethodinv
    TONE,      // EvlocallabshowmaskexpMethodinv
    TONE,      // EvlocallabshowmaskSHMethodinv
    TONE,      // Evlocallabclarilres
    TONE,      // Evlocallabclarisoft
    TONE,      // Evlocallabclaricres
    TONE,      // Evlocallabresidchro
    TONE,      // Evlocallabgamm
    TONE,      // Evlocallabfatamount
    TONE,      // Evlocallabfatdetail
    TONE,      // Evlocallabfatanchor
    TONE,      // Evlocallabfatlevel
    TONE,      // EvlocallabSpotCreated
    TONE,      // EvlocallabexnoiseMethod
    TONE,      // Evlocallabdepth
    TONE,      // Evlocallabloglin
    TONE,      // EvlocallabdehazeSaturation
    TONE,      // Evlocallaboffs
    TONE,      // EvlocallabCTtransCurve
    TONE,      // Evlocallabcliptm
    TONE,      // Evlocallabenatmmaskaft
    TONE,      // EvlocallabenaExpmaskaft
    TONE,      // Evlocallablapmasktm
    TONE,      // Evlocallablapmaskreti
    TONE,      // Evlocallablapmaskexp
    TONE,      // Evlocallablapmaskcol
    TONE,      // EvlocallablapmaskSH
    TONE,      // Evlocallablapmaskcb
    TONE,      // Evlocallablapmaskbl
    TONE,      // Evlocallablaplac
    TONE,      // Evlocallabdetailthr
    TONE,      // Evlocallabfftwbl
    TONE,      // Evlocallabisogr
    TONE,      // Evlocallabstrengr
    TONE,      // Evlocallabscalegr
    TONE,      // EvlocallabLmaskshape
    TONE,      // EvlocallabLmaskexpshape
    TONE,      // EvlocallabLmaskSHshape
    TONE,      // EvlocallabLmasktmshape
    TONE,      // EvlocallabLmaskretishape
    TONE,      // EvlocallabLmaskcbshape
    TONE,      // EvlocallabLmaskblshape
    TONE,      // EvlocallabLLmaskblshapewav
    TONE,      // Evlocallabshadmaskbl
    TONE,      // EvlocallabLLmaskcolshapewav
    TONE,      // Evlocallabshadmaskcol
    TONE,      // EvlocallabcsThreshold
    TONE,      // EvlocallabcsThresholdblur
    TONE,      // EvlocallabcsThresholdcol
    TONE,      // Evlocallabdeltae
    TONE,      // EvLocallabSpotscopemask
    TONE,      // EvlocallabshMethod
    TONE,      // EvlocallabEqualizersh
    TONE,      // EvlocallabdetailSH
    TONE,      // EvlocallabfatamountSH
    TONE,      // EvlocallabfatanchorSH
    TONE,      // Evlocallabshortc
    TONE,      // EvLocallabSpotlumask
    TONE,      // EvlocallabgamSH
    TONE,      // EvlocallabsloSH
    TONE,      // Evlocallabsavrest
    TONE,      // Evlocallabrecurs
    TONE,      // EvLocallabmergecolMethod
    TONE,      // EvLocallabopacol
    TONE,      // Evlocallabrgbshape
    TONE,      // EvLocallabtoneMethod
    TONE,      // EvLocallabspecial
    TONE,      // EvLocallabconthrcol
    TONE,      // EvLocallabmerMethod
    TONE,      // EvLocallabstrumaskcol
    TONE,      // EvLocallabstrumaskbl
    TONE,      // EvLocallabtoolcol
    TONE,      // Evlocallabtoolbl
    TONE,      // EvlocallabHHhmaskshape
    TONE,      // EvlocallabCCmaskvibshape
    TONE,      // EvlocallabLLmaskvibshape
    TONE,      // EvlocallabHHmaskvibshape
    TONE,      // EvlocallabshowmaskvibMethod
    TONE,      // EvLocallabEnavibMask
    TONE,      // Evlocallabblendmaskvi
    TONE,      // Evlocallabradmaskvib
    TONE,      // Evlocallabchromaskvib
    TONE,      // Evlocallabgammaskvib
    TONE,      // Evlocallabslomaskvib
    TONE,      // Evlocallablapmaskvib
    TONE,      // EvlocallabLmaskvibshape
    TONE,      // EvLocallabLabGridmergValue
    TONE,      // EvLocallabmercol
    TONE,      // EvLocallabmerlucol
    TONE,      // Evlocallabstrmaskexp
    TONE,      // Evlocallabangmaskexp
    TONE,      // Evlocallabstrexp
    TONE,      // Evlocallabangexp
    AUTOEXP | M_AUTOEXP,   // EvlocallabstrSH
    TONE,      // EvlocallabangSH
    TONE,      // Evlocallabstrcol
    TONE,      // Evlocallabangcol
    TONE,      // Evlocallabstrcolab
    TONE,      // EvLocallabSpotfeather
    TONE,      // Evlocallabstrcolh
    TONE,      // Evlocallabstrvib
    TONE,      // Evlocallabangvib
    TONE,      // Evlocallabstrvibab
    TONE,      // Evlocallabstrvibh
    TONE,      // EvLocallabSpotcomplexMethod
    TONE,      // Evlocallabclshape
    TONE,      // Evlocallablcshape
    TONE,      // Evlocallabblurcol
    TONE,      // Evlocallabcontcol
    TONE,      // EvLocallabfftColorMask
    AUTOEXP | M_AUTOEXP,   // EvLocenalog
    HDR,          // EvLocallabAuto
    TONE,      // EvlocallabsourceGray
    0,          // EvlocallabsourceGrayAuto : obsolete
    HDR,          // EvlocallabAutoGray
    TONE,      // EvlocallabblackEv
    TONE,      // EvlocallabwhiteEv
    TONE,      // EvlocallabtargetGray
    TONE,      // Evlocallabdetail
    TONE,      // Evlocallabsensilog
    TONE,             // Evlocallabfullimage
    TONE,      // Evlocallabbaselog
    TONE,      // Evlocallabresidblur
    TONE,      // Evlocallabblurlc
    TONE,      // Evlocallablevelblur
    TONE,      // EvlocallabwavCurvelev
    TONE,      // EvlocallabwavCurvecon
    TONE,      // Evlocallabsigma
    TONE,      // Evlocallaboriglc
    TONE,      // Evlocallabsigmadc
    TONE,      // Evlocallabdeltad
    TONE,      // EvlocallabwavCurvecomp
    TONE,      // Evlocallabfatres
    TONE,      // EvLocallabSpotbalanh
    TONE,      // EvlocallabwavCurveden
    TONE,      // EvlocallabHHmasklcshape
    TONE,      // EvlocallabCCmasklcshape
    TONE,      // EvlocallabLLmasklcshape
    TONE,      // EvlocallabEnalcMask
    TONE,      // EvlocallabshowmasklcMethod
    TONE,      // Evlocallabblendmasklc
    TONE,      // Evlocallabradmasklc
    TONE,      // Evlocallabchromasklc
    TONE,      // EvlocallabLmasklcshape
    TONE,      // Evlocallabchromalev
    TONE,      // Evlocallabchromablu
    TONE,      // Evlocallaboffset
    TONE,      // Evlocallabwavblur
    TONE,      // Evlocallabwavcont
    TONE,      // Evlocallabwavcomp
    TONE,      // Evlocallabwavcompre
    TONE,      // EvlocallabwavCurvecompre
    TONE,      // Evlocallabresidcomp
    TONE,      // Evlocallabthreswav
    TONE,      // Evlocallabstrwav
    TONE,      // Evlocallabangwav
    TONE,      // Evlocallabwavgradl
    TONE,      // Evlocallabstrlog
    TONE,      // Evlocallabanglog
    TONE,      // EvLocallabSpotcolorde
    TONE,      // EvlocallabshowmasksharMethod
    TONE,      // Evlocallabshowreset
    TONE,      // Evlocallabstrengthw
    TONE,      // Evlocallabradiusw
    TONE,      // Evlocallabdetailw
    TONE,      // Evlocallabgradw
    TONE,      // Evlocallabtloww
    TONE,      // Evlocallabthigw
    TONE,      // EvlocallabwavCurveedg
    TONE,      // EvlocallablocaledgMethod
    TONE,      // Evlocallabwavedg
    TONE,      // Evlocallabedgw
    TONE,      // Evlocallabbasew
    TONE,      // EvlocallablocalneiMethod
    TONE,      // Evlocallabwaveshow
    TONE,      // EvLocallabSpotwavMethod
    TONE,      // EvlocallabchroMethod
    TONE,      // Evlocallabstrbl
    TONE,      // Evlocallabsigmadr
    TONE,      // Evlocallabsigmabl
    TONE,      // Evlocallabsigmaed
    TONE,      // Evlocallabresidsha
    TONE,      // Evlocallabresidshathr
    TONE,      // Evlocallabresidhi
    TONE,      // Evlocallabresidhithr
    TONE,      // Evlocallabsigmalc
    TONE,      // Evlocallabsigmalc2
    TONE,      // Evlocallabblwh
    TONE,      // EvlocallabcomplexityWithRefresh
    0,                // can be reused
    TONE,      // EvLocallabSpotcolorscope
    TONE,      // EvlocallabshowmasktypMethod
    TONE,      // Evlocallabshadmaskblsha
    TONE,      // EvLocenamask
    TONE,      // Evlocallabsensimask
    TONE,      // Evlocallabblendmask
    TONE,      // EvLocallabEna_Mask
    TONE,      // Evlocallabradmask
    TONE,      // Evlocallablapmask
    TONE,      // Evlocallabchromask
    TONE,      // Evlocallabgammask
    TONE,      // Evlocallabslopmask
    TONE,      // EvlocallabCCmask_shape
    TONE,      // EvlocallabLLmask_shape
    TONE,      // EvlocallabHHmask_shape
    TONE,      // EvLocallabtoolmask
    TONE,      // Evlocallabstrumaskmask
    TONE,      // EvlocallabHHhmask_shape
    TONE,      // EvLocallabfftmask
    TONE,      // Evlocallabblurmask
    TONE,      // Evlocallabcontmask
    TONE,      // Evlocallabshadmask
    TONE,      // EvlocallabLmask_shape
    TONE,      // EvlocallabLLmask_shapewav
    TONE,      // EvlocallabcsThresholdmask
    TONE,      // Evlocallabstr_mask
    TONE,      // Evlocallabang_mask
    TONE,      // Evlocallabsoftradiusmask
    TONE,      // Evlocallabblendmaskab
    TONE,      // EvLocallabSpotprevMethod
    TONE,      // Evlocallabactiv
    TONE,      // EvlocallabCHshape
    TONE,      //EvlocallabquaMethod
    TONE,      //Evlocallabhishow
    TONE,      // Evlocallabinvbl
    TONE,      // Evlocallabcatad
    TONE,      // Evlocallabciecam
    TONE,      // Evlocallabsourceabs
    TONE,      // Evlocallabtargabs
    TONE,      // Evlocallabsurround
    TONE,      // Evlocallabsaturl
    TONE,      // Evlocallabcontl
    TONE,      //EvlocallabCCmaskshapeL 
    TONE,      //EvlocallabLLmaskshapeL
    TONE,      // EvlocallabHHmaskshapeL
    TONE,      // EvlocallabenaLMask
    TONE,      // EvlocallabblendmaskL
    TONE,      // EvlocallabradmaskL
    TONE,      // EvlocallabchromaskL
    TONE,      //EvlocallabLmaskshapeL
    TONE,      // Evlocallablightl
    TONE,      // EvlocallabLshapeL
    TONE,      // Evlocallabcontq
    TONE,      // Evlocallabsursour
    TONE,      // Evlocallablightq
    TONE,      // Evlocallabcolorfl
    TONE,      // Evlocallabrepar
    TONE,      //EvlocallabwavCurvehue
    TONE,      // Evlocallablevelthr
    TONE,      // Evlocallablevelthrlow
    TONE,      //Evlocallabusemask1
    TONE,      // Evlocallablnoiselow
    TONE,      // Evlocallabrecothres
    TONE,      // Evlocallablowthres
    TONE,      // Evlocallabhigthres
    TONE,      // Evlocallabrecothresd
    TONE,      // Evlocallablowthresd
    TONE,      // Evlocallabhigthresd
    TONE,      // Evlocallabinvmaskd
    TONE,      // Evlocallabinvmask
    TONE,      // Evlocallabdecayd
    TONE,      // Evlocallabrecothresc
    TONE,      // Evlocallablowthresc
    TONE,      // Evlocallabhigthresc
    TONE,      // Evlocallabdecayc
    TONE,      // Evlocallabmidthresd
    TONE,      // Evlocallabrecothresl
    TONE,      // Evlocallablowthresl
    TONE,      // Evlocallabhigthresl
    TONE,     // Evlocallabdecayl
    TONE,      // Evlocallabrecothrese
    TONE,      // Evlocallablowthrese
    TONE,      // Evlocallabhigthrese
    TONE,      // Evlocallabdecaye
    TONE,      // Evlocallabrecothress
    TONE,      // Evlocallablowthress
    TONE,      // Evlocallabhigthress
    TONE,      // Evlocallabdecays
    TONE,      // Evlocallabrecothrev
    TONE,      // Evlocallablowthresv
    TONE,      // Evlocallabhigthresv
    TONE,      // Evlocallabdecayv
    TONE,      // Evlocallabrecothrew
    TONE,      // Evlocallablowthresw
    TONE,      // Evlocallabhigthresw
    TONE,      // Evlocallabdecayw
    TONE,      // Evlocallabmidthresdch
    TONE,      // Evlocallabrecothret
    TONE,      // Evlocallablowthrest
    TONE,      // Evlocallabhigthrest
    TONE,      // Evlocallabdecayt
    TONE,      // Evlocallabrecothrecb
    TONE,      // Evlocallablowthrescb
    TONE,      // Evlocallabhigthrescb
    TONE,      // Evlocallabdecaycb
    TONE,      // Evlocallabrecothrer
    TONE,      // Evlocallablowthresr
    TONE,      // Evlocallabhigthresr
    TONE,      // Evlocallabdecayr
    TONE,      // Evlocallabnlstr
    TONE,      // Evlocallabnldet
    TONE,      // Evlocallabnlpat
    TONE,      // Evlocallabnlrad
    TONE,      // Evlocallabnlgam
    TONE,      // Evlocallabdivgr
    TONE,      // EvLocallabSpotavoidrad
    TONE,      // EvLocallabSpotavoidmun
    TONE,      // Evlocallabcontthres
    TONE,      // Evlocallabnorm
    TONE,      // Evlocallabreparw
    TONE,      // Evlocallabreparcol
    TONE,      // Evlocallabreparden
    TONE,      // Evlocallabreparsh
    TONE,      // Evlocallabreparexp
    TONE,      // Evlocallabrepartm
    TONE,      // Evlocallabchroml
    TONE,      // Evlocallabresidgam
    TONE,      // Evlocallabresidslop
    TONE,      // Evlocallabnoisegam
    TONE,       //Evlocallabgamlc
    TONE,       //Evlocallabgamc
    TONE,       //Evlocallabgamex
    AUTOEXP | M_AUTOEXP,    // EvLocenacie
    TONE,        //Evlocallabreparcie
    HDR,     //EvlocallabAutograycie
    HDR,    //EvlocallabsourceGraycie
    HDR,    //Evlocallabsourceabscie
    TONE,       //Evlocallabsursourcie
    TONE,       //Evlocallabsaturlcie
    TONE,       //Evlocallabchromlcie
    TONE,       //Evlocallablightlcie
    TONE,       //Evlocallablightqcie
    TONE,       //Evlocallabcontlcie
    TONE,       //Evlocallabcontthrescie
    TONE,       //Evlocallabcontqcie
    TONE,       //Evlocallabcolorflcie
    TONE,       //Evlocallabtargabscie
    TONE,       //EvlocallabtargetGraycie
    TONE,       //Evlocallabcatadcie
    TONE,       //Evlocallabdetailcie
    TONE,       //Evlocallabsurroundcie
    TONE,       //Evlocallabsensicie
    TONE,       //Evlocallabmodecie
    TONE,       //Evlocallabrstprotectcie
    TONE,       //Evlocallabsigmoidldacie12
    TONE,       //Evlocallabsigmoidthcie12
    TONE,       //Evlocallabsigmoidblcie12
    HDR,    //Evlocallabcomprcieauto
    TONE,       //Evlocallabhuecie
    TONE,       //Evlocallabjabcie
    TONE,       //Evlocallablightjzcie
    TONE,       //Evlocallabcontjzcie
    TONE,       //Evlocallabchromjzcie
    TONE,       //Evlocallabhuejzcie
    TONE,       //Evlocallabsigmoidldajzcie12
    TONE,       //Evlocallabsigmoidthjzcie12
    TONE,       //Evlocallabsigmoidbljzcie12
    TONE,       //Evlocallabadapjzcie
    TONE,       //Evlocallabmodecam
    TONE,       //Evlocallabhljzcie
    TONE,       //Evlocallabhlthjzcie
    TONE,       //Evlocallabshjzcie
    TONE,       //Evlocallabshthjzcie
    TONE,       //Evlocallabradjzcie
//    AUTOEXP,    //EvlocallabHHshapejz
    TONE,       //EvlocallabCHshapejz
    TONE,       //Evlocallabjz100
    TONE,       //Evlocallabpqremap
    TONE,       //EvlocallabLHshapejz
    TONE,       //Evlocallabshargam
    TONE,       //Evlocallabvibgam
    TONE,       //EvLocallabtoneMethodcie
    TONE,       //Evlocallabshapecie
    TONE,       //EvLocallabtoneMethodcie2
    TONE,       //Evlocallabshapecie2
    TONE,       //Evlocallabshapejz
    TONE,       //Evlocallabshapecz
    TONE,       //Evlocallabshapeczjz
    0,          // unused (Evlocallabforcejz = 1111 is commented out in procevents.h, but the slot remains)
//    AUTOEXP,    //Evlocallablightlzcam
//    AUTOEXP,    //Evlocallablightqzcam
//    AUTOEXP,    //Evlocallabcontlzcam
//    AUTOEXP,    //Evlocallabcontqzcam
//    AUTOEXP,    //Evlocallabcontthreszcam
//    AUTOEXP,    //Evlocallabcolorflzcam
//    AUTOEXP,    //Evlocallabsaturzcam
//    AUTOEXP,    //Evlocallabchromzcam
    TONE,       //Evlocallabpqremapcam16
    TONE,       //EvLocallabEnacieMask    
    TONE,       //EvlocallabCCmaskcieshape
    TONE,       //EvlocallabLLmaskcieshape
    TONE,       //EvlocallabHHmaskcieshape
    TONE,       //Evlocallabblendmaskcie
    TONE,       //Evlocallabradmaskcie
    TONE,       //Evlocallabchromaskcie
    TONE,       //EvlocallabLmaskcieshape 
    TONE,       //Evlocallabrecothrescie
    TONE,       //Evlocallablowthrescie
    TONE,       //Evlocallabhigthrescie
    TONE,       //Evlocallabdecaycie
    TONE,       //Evlocallablapmaskcie
    TONE,       //Evlocallabgammaskcie
    TONE,       //Evlocallabslomaskcie
    TONE,       //Evlocallabqtoj
    TONE,       //Evlocallabsaturjzcie
    TONE,       //EvLocallabSpotdenoichmask
    TONE,       //Evlocallabsigmalcjz
    TONE,       //EvlocallabcsThresholdjz
    TONE,       //EvlocallabwavCurvejz
    TONE,       //Evlocallabclarilresjz
    TONE,       //Evlocallabclaricresjz
    TONE,       //Evlocallabclarisoftjz
    TONE,       //EvlocallabHHshapejz
    TONE,       //Evlocallabsoftjzcie
    TONE,       //Evlocallabthrhjzcie
    TONE,       //Evlocallabchjzcie
    TONE,       //Evlocallabstrsoftjzcie
    TONE,       //EvlocallabblackEvjz
    TONE,       //EvlocallabwhiteEvjz
    TONE,       //Evlocallablogjz
    TONE,       //Evlocallabtargetjz
    TONE,       //Evlocallabsigybjz12
    TONE,       //Evlocallabsigjz12
    TONE,       //Evlocallabsigq12
    TONE,       //Evlocallablogcie
    AIDENOISE,  //EvAIDNEnabled
    AIDENOISE,  //EvAIDNBlend
    LUMINANCECURVE,  //EvColorGradingEnabled
    LUMINANCECURVE,  //EvColorGradingShadows
    LUMINANCECURVE,  //EvColorGradingMidtones
    LUMINANCECURVE,  //EvColorGradingHighlights
    LUMINANCECURVE,  //EvColorGradingGlobal
    LUMINANCECURVE,  //EvColorGradingBlending
    LUMINANCECURVE,  //EvColorGradingBalance
    // HSV and Point Color run inside rgbProc: the footing the stock HSV
    // events have (see TONE in refreshmap.h), not a rebuild from the source.
    TONE,            //EvHSVEqMode
    TONE,            //EvHSVEqSliders
    TONE,            //EvLocallabSpotGradAngle
    TONE,            //EvPointColorEnabled
    TONE,            //EvPointColorHue
    TONE,            //EvPointColorHueShift
    TONE,            //EvPointColorSaturation
    TONE,            //EvPointColorLuminance
    TONE,            //EvPointColorRange
    TONE,            //EvPointColorTargetAdd
    TONE,            //EvRGBMasterCurve
    TONE,            //EvBWNeutrals
    TONE,            //EvBWTone
    TONE          //EvBWStrength
};

// The table is positional: one entry per event code, gaps included. A missing
// or extra line shifts every later event onto its neighbour's action without
// any other symptom, so the length is checked here.
static_assert(sizeof(refreshmap) / sizeof(refreshmap[0]) == rtengine::NUMOFEVENTS,
              "refreshmap[] must have exactly one entry per ProcEventCode");


namespace rtengine
{

RefreshMapper::RefreshMapper():
    next_event_(rtengine::NUMOFEVENTS)
{
    // Verification switch: STEEP_LEGACY_TONE=1 puts the tone events back on
    // the old AUTOEXP action, so a single binary can render the same edit
    // both ways and the two frame dumps can be compared pixel for pixel.
    const char* const legacyTone = std::getenv("STEEP_LEGACY_TONE");
    const bool useLegacyTone = legacyTone != nullptr && legacyTone[0] != '\0' && legacyTone[0] != '0';

    for (int event = 0; event < rtengine::NUMOFEVENTS; ++event) {
        int action = refreshmap[event];

        if (useLegacyTone && action == TONE) {
            action = AUTOEXP;
        }

        actions_[event] = action;
    }

    if (useLegacyTone) {
        std::fprintf(stderr, "STEEP_LEGACY_TONE: tone events mapped back to AUTOEXP\n");
    }
}


ProcEvent RefreshMapper::newEvent()
{
    return ProcEvent(++next_event_);
}


void RefreshMapper::mapEvent(ProcEvent event, int action)
{
    actions_[event] = action;
}


int RefreshMapper::getAction(ProcEvent event) const
{
    auto it = actions_.find(event);

    if (it == actions_.end()) {
        return 0;
    } else {
        return it->second;
    }
}


RefreshMapper *RefreshMapper::getInstance()
{
    static RefreshMapper instance;
    return &instance;
}

} // namespace rtengine
