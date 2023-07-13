// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

///
/// @file   CalculatedEdx.cxx
/// @author Tuba Gündem, tuba.gundem@cern.ch
///

#include "TPCCalibration/CalculatedEdx.h"
#include "TPCBase/PadPos.h"
#include "TPCBase/ROC.h"
#include "TPCBase/Mapper.h"
#include "DataFormatsTPC/ClusterNative.h"
#include "DetectorsBase/Propagator.h"
#include "CCDB/BasicCCDBManager.h"
#include "TPCBase/CDBInterface.h"
#include "TPCReconstruction/TPCFastTransformHelperO2.h"
#include "TPCCalibration/CalibPadGainTracksBase.h"
#include "CalibdEdxTrackTopologyPol.h"
#include "DataFormatsParameters/GRPMagField.h"

using namespace o2::tpc;

CalculatedEdx::CalculatedEdx()
{
  mTPCCorrMapsHelper.setOwner(true);
  mTPCCorrMapsHelper.setCorrMap(TPCFastTransformHelperO2::instance()->create(0));
}

void CalculatedEdx::setMembers(std::vector<o2::tpc::TPCClRefElem>* tpcTrackClIdxVecInput, const o2::tpc::ClusterNativeAccess& clIndex, std::vector<o2::tpc::TrackTPC>* vTPCTracksArrayInp)
{
  mTracks = vTPCTracksArrayInp;
  mTPCTrackClIdxVecInput = tpcTrackClIdxVecInput;
  mClusterIndex = &clIndex;
}

void CalculatedEdx::setRefit()
{
  mRefit = std::make_unique<o2::gpu::GPUO2InterfaceRefit>(mClusterIndex, &mTPCCorrMapsHelper, mField, mTPCTrackClIdxVecInput->data(), nullptr, mTracks);
}

void CalculatedEdx::fillMissingClusters(std::array<std::vector<float>, 5> chargeROC, int missingClusters[4], float minCharge, int method)
{
  // adding minimum charge
  if (method == 0) {
    for (int roc = 0; roc < 4; roc++) {
      for (int i = 0; i < missingClusters[roc]; i++) {
        chargeROC[roc].emplace_back(minCharge);
      }
    }
  }
  // adding minimum charge/2
  if (method == 1) {
    for (int roc = 0; roc < 4; roc++) {
      for (int i = 0; i < missingClusters[roc]; i++) {
        float result = minCharge / 2;
        chargeROC[roc].emplace_back(result);
      }
    }
  }
}

void CalculatedEdx::calculatedEdx(o2::tpc::TrackTPC& track, dEdxInfo& output, float low, float high, CorrectionFlags mask)
{
  // get number of clusters
  const int nClusters = track.getNClusterReferences();

  std::array<std::vector<float>, 5> chargeTotROC;
  std::array<std::vector<float>, 5> chargeMaxROC;
  int nClsROC[4] = {0};
  int nClsSubThreshROC[4] = {0};

  std::vector<int> regionVector;             ///< debug streamer vector for region
  std::vector<unsigned char> rowIndexVector; ///< debug streamer vector for row index
  std::vector<unsigned char> padVector;      ///< debug streamer vector for pad
  std::vector<int> stackVector;              ///< debug streamer vector for stack
  std::vector<unsigned char> sectorVector;   ///< debug streamer vector for sector

  std::vector<float> topologyCorrVector;     ///< debug streamer vector for simple topology correction
  std::vector<float> topologyCorrTotVector;  ///< debug streamer vector for polynomial topology correction
  std::vector<float> topologyCorrMaxVector;  ///< debug streamer vector for polynomial topology correction
  std::vector<float> gainVector;             ///< debug streamer vector for gain
  std::vector<float> residualCorrTotVector;  ///< debug streamer vector for residual dEdx correction
  std::vector<float> residualCorrMaxVector;  ///< debug streamer vector for residual dEdx correction

  std::vector<float> chargeTotVector;        ///< debug streamer vector for charge
  std::vector<float> chargeMaxVector;        ///< debug streamer vector for charge

  if (mDebug) {
    regionVector.reserve(nClusters);
    rowIndexVector.reserve(nClusters);
    padVector.reserve(nClusters);
    stackVector.reserve(nClusters);
    sectorVector.reserve(nClusters);
    topologyCorrVector.reserve(nClusters);
    topologyCorrTotVector.reserve(nClusters);
    topologyCorrMaxVector.reserve(nClusters);
    gainVector.reserve(nClusters);
    residualCorrTotVector.reserve(nClusters);
    residualCorrMaxVector.reserve(nClusters);
    chargeTotVector.reserve(nClusters);
    chargeMaxVector.reserve(nClusters);
  }

  // for missing clusters
  unsigned char rowIndexOld = 0;
  unsigned char sectorIndexOld = 0;
  float minChargeTot = 100000.f;
  float minChargeMax = 100000.f;

  // loop over the clusters
  for (int iCl = 0; iCl < nClusters; iCl++) {

    const o2::tpc::ClusterNative& cl = track.getCluster(*mTPCTrackClIdxVecInput, iCl, *mClusterIndex);

    unsigned char sectorIndex = 0;
    unsigned char rowIndex = 0;
    unsigned int clusterIndexNumb = 0;

    // set sectorIndex, rowIndex, clusterIndexNumb
    track.getClusterReference(*mTPCTrackClIdxVecInput, iCl, sectorIndex, rowIndex, clusterIndexNumb);

    // get x position of the track
    float xPosition = Mapper::instance().getPadCentre(PadPos(rowIndex, 0)).X();

    bool check = true;
    if (!mPropagateTrack) {
      if (mRefit == nullptr) {
        LOGP(error, "mRefit is a nullptr, call the function setRefit() before looping over the tracks.");
      }
      mRefit->setTrackReferenceX(xPosition);
      check = (mRefit->RefitTrackAsGPU(track, false, true) < 0) ? false : true;
    } else {
      // propagate this track to the plane X=xk (cm) in the field "b" (kG)
      check = o2::base::Propagator::Instance()->PropagateToXBxByBz(track, xPosition, 0.9f, 2., o2::base::Propagator::MatCorrType::USEMatCorrLUT);
    }

    if (!check || std::isnan(track.getParam(1))) {
      continue;
    }

    // get region and charge value
    const int region = Mapper::REGION[rowIndex];
    float chargeTot = cl.qTot;
    float chargeMax = cl.qMax;

    // get pad and threshold
    const unsigned char pad = std::clamp(static_cast<unsigned int>(cl.getPad() + 0.5f), static_cast<unsigned int>(0), Mapper::PADSPERROW[region][Mapper::getLocalRowFromGlobalRow(rowIndex)] - 1); // the left side of the pad is defined at e.g. 3.5 and the right side at 4.5
    const float threshold = mCalibCont.getZeroSupressionThreshold(sectorIndex, rowIndex, pad);

    // get stack and stack ID
    const CRU cru(Sector(sectorIndex), region);
    const auto stack = cru.gemStack();
    StackID stackID{sectorIndex, stack};

    // find missing clusters
    int missingClusters = rowIndexOld - rowIndex - 1;
    if ((missingClusters == mMaxMissingCl) && (sectorIndexOld == sectorIndex)) {
      if (stack == GEMstack::IROCgem) {
        nClsSubThreshROC[0] += missingClusters;
      } else if (stack == GEMstack::OROC1gem) {
        nClsSubThreshROC[1] += missingClusters;
      } else if (stack == GEMstack::OROC2gem) {
        nClsSubThreshROC[2] += missingClusters;
      } else if (stack == GEMstack::OROC3gem) {
        nClsSubThreshROC[3] += missingClusters;
      }
    };
    rowIndexOld = rowIndex;
    sectorIndexOld = sectorIndex;

    // get effective length
    float effectiveLength = 1.0f;
    float effectiveLengthTot = 1.0f;
    float effectiveLengthMax = 1.0f;
    if ((mask & CorrectionFlags::TopologySimple) == CorrectionFlags::TopologySimple) {
      effectiveLength = getTrackTopologyCorrection(track, region);
      chargeTot /= effectiveLength;
      chargeMax /= effectiveLength;
    };
    if ((mask & CorrectionFlags::TopologyPol) == CorrectionFlags::TopologyPol) {
      effectiveLengthTot = getTrackTopologyCorrectionPol(track, cl, region, chargeTot, ChargeType::Tot, threshold);
      effectiveLengthMax = getTrackTopologyCorrectionPol(track, cl, region, chargeMax, ChargeType::Max, threshold);
      chargeTot /= effectiveLengthTot;
      chargeMax /= effectiveLengthMax;
    };

    // get gain
    float gain = 1.0f;
    if ((mask & CorrectionFlags::GainFull) == CorrectionFlags::GainFull) {
      gain = mCalibCont.getGain(sectorIndex, rowIndex, pad);
    };
    if ((mask & CorrectionFlags::GainResidual) == CorrectionFlags::GainResidual) {
      gain *= mCalibCont.getResidualGain(sectorIndex, rowIndex, pad);
    };
    chargeTot /= gain;
    chargeMax /= gain;

    // get dEdx correction on tgl and sector plane
    float corrTot = 1.0f;
    float corrMax = 1.0f;
    if ((mask & CorrectionFlags::dEdxResidual) == CorrectionFlags::dEdxResidual) {
      corrTot = mCalibCont.getResidualCorrection(stackID, ChargeType::Tot, track.getTgl(), track.getSnp());
      corrMax = mCalibCont.getResidualCorrection(stackID, ChargeType::Max, track.getTgl(), track.getSnp());
      chargeTot /= corrTot;
      chargeMax /= corrMax;
    };

    // set the min charge
    if (chargeTot < minChargeTot) {
      minChargeTot = chargeTot;
    };

    if (chargeMax < minChargeMax) {
      minChargeMax = chargeMax;
    };

    if (stack == GEMstack::IROCgem) {
      chargeTotROC[0].emplace_back(chargeTot);
      chargeMaxROC[0].emplace_back(chargeMax);
      nClsROC[0]++;
    } else if (stack == GEMstack::OROC1gem) {
      chargeTotROC[1].emplace_back(chargeTot);
      chargeMaxROC[1].emplace_back(chargeMax);
      nClsROC[1]++;
    } else if (stack == GEMstack::OROC2gem) {
      chargeTotROC[2].emplace_back(chargeTot);
      chargeMaxROC[2].emplace_back(chargeMax);
      nClsROC[2]++;
    } else if (stack == GEMstack::OROC3gem) {
      chargeTotROC[3].emplace_back(chargeTot);
      chargeMaxROC[3].emplace_back(chargeMax);
      nClsROC[3]++;
    };

    chargeTotROC[4].emplace_back(chargeTot);
    chargeMaxROC[4].emplace_back(chargeMax);

    // for debugging
    if (mDebug) {
      // mapping for the stack info
      std::map<o2::tpc::GEMstack, int> map;
      map[GEMstack::IROCgem] = 0;
      map[GEMstack::OROC1gem] = 1;
      map[GEMstack::OROC2gem] = 2;
      map[GEMstack::OROC3gem] = 3;

      // filling debug vectors
      regionVector.emplace_back(region);
      rowIndexVector.emplace_back(rowIndex);
      padVector.emplace_back(pad);
      stackVector.emplace_back(map[stack]);
      sectorVector.emplace_back(sectorIndex);

      topologyCorrVector.emplace_back(effectiveLength);
      topologyCorrTotVector.emplace_back(effectiveLengthTot);
      topologyCorrMaxVector.emplace_back(effectiveLengthMax);
      gainVector.emplace_back(gain);
      residualCorrTotVector.emplace_back(corrTot);
      residualCorrMaxVector.emplace_back(corrMax);

      chargeTotVector.emplace_back(chargeTot);
      chargeMaxVector.emplace_back(chargeMax);
    };
  }

  // number of clusters
  output.NHitsIROC = nClsROC[0];
  output.NHitsOROC1 = nClsROC[1];
  output.NHitsOROC2 = nClsROC[2];
  output.NHitsOROC3 = nClsROC[3];

  // number of missing clusters
  output.NHitsSubThresholdIROC = nClsSubThreshROC[0];
  output.NHitsSubThresholdOROC1 = nClsSubThreshROC[1];
  output.NHitsSubThresholdOROC2 = nClsSubThreshROC[2];
  output.NHitsSubThresholdOROC3 = nClsSubThreshROC[3];

  fillMissingClusters(chargeTotROC, nClsROC, minChargeTot, 1);
  fillMissingClusters(chargeMaxROC, nClsROC, minChargeMax, 1);

  // calculate dEdx
  output.dEdxTotIROC = getTruncMean(chargeTotROC[0], low, high);
  output.dEdxTotOROC1 = getTruncMean(chargeTotROC[1], low, high);
  output.dEdxTotOROC2 = getTruncMean(chargeTotROC[2], low, high);
  output.dEdxTotOROC3 = getTruncMean(chargeTotROC[3], low, high);
  output.dEdxTotTPC = getTruncMean(chargeTotROC[4], low, high);

  output.dEdxMaxIROC = getTruncMean(chargeMaxROC[0], low, high);
  output.dEdxMaxOROC1 = getTruncMean(chargeMaxROC[1], low, high);
  output.dEdxMaxOROC2 = getTruncMean(chargeMaxROC[2], low, high);
  output.dEdxMaxOROC3 = getTruncMean(chargeMaxROC[3], low, high);
  output.dEdxMaxTPC = getTruncMean(chargeMaxROC[4], low, high);

  // for debugging
  if (mDebug) {
    if (mStreamer == nullptr) {
      setStreamer();
    }

    (*mStreamer) << "dEdxDebug"
                 << "trackVector=" << &track
                 << "regionVector=" << regionVector
                 << "rowIndexVector=" << rowIndexVector
                 << "padVector=" << padVector
                 << "stackVector=" << stackVector
                 << "sectorVector=" << sectorVector
                 << "topologyCorrVector=" << topologyCorrVector
                 << "topologyCorrTotVector=" << topologyCorrTotVector
                 << "topologyCorrMaxVector=" << topologyCorrMaxVector
                 << "gainVector=" << gainVector
                 << "residualCorrTotVector=" << residualCorrTotVector
                 << "residualCorrMaxVector=" << residualCorrMaxVector
                 << "chargeTotVector=" << chargeTotVector
                 << "chargeMaxVector=" << chargeMaxVector
                 << "minChargeTot=" << minChargeTot
                 << "minChargeMax=" << minChargeMax
                 << "output=" << output
                 << "\n";
  }
}

float CalculatedEdx::getTruncMean(std::vector<float>& charge, float low, float high)
{
  // sort the charge vector
  std::sort(charge.begin(), charge.end());

  // calculate truncated mean
  int nCl = 0;
  float sum = 0;
  size_t firstCl = charge.size() * low;
  size_t lastCl = charge.size() * high;

  for (size_t iCl = firstCl; iCl < lastCl; ++iCl) {
    sum += charge[iCl];
    ++nCl;
  }

  if (nCl > 0) {
    sum /= nCl;
  }
  return sum;
}

float CalculatedEdx::getTrackTopologyCorrection(const o2::tpc::TrackTPC& track, const unsigned int region) const
{
  const float padLength = Mapper::instance().getPadRegionInfo(region).getPadHeight();
  const float snp = track.getSnp();
  const float tgl = track.getTgl();
  const float snp2 = snp * snp;
  const float tgl2 = tgl * tgl;
  // calculate the trace length of the track over the pad
  const float effectiveLength = padLength * std::sqrt((1 + tgl2) / (1 - snp2));
  return effectiveLength;
}

float CalculatedEdx::getTrackTopologyCorrectionPol(const o2::tpc::TrackTPC& track, const o2::tpc::ClusterNative& cl, const unsigned int region, const float charge, ChargeType chargeType, const float threshold) const
{
  const float snp = std::abs(track.getSnp());
  const float tgl = track.getTgl();
  const float snp2 = snp * snp;
  const float tgl2 = tgl * tgl;
  const float sec2 = 1.f / (1.f - snp2);
  const float tanTheta = std::sqrt(tgl2 * sec2);

  const float z = std::abs(track.getParam(1));
  const float padTmp = cl.getPad();
  const float absRelPad = std::abs(padTmp - int(padTmp + 0.5f));
  const float relTime = cl.getTime() - int(cl.getTime() + 0.5f);

  const float effectiveLength = mCalibCont.getTopologyCorrection(region, chargeType, tanTheta, snp, z, absRelPad, relTime, threshold, charge);
  return effectiveLength;
}

void CalculatedEdx::loadCalibsFromCCDB(int runNumber)
{
  // setup CCDB manager
  auto& cm = o2::ccdb::BasicCCDBManager::instance();
  cm.setURL("http://alice-ccdb.cern.ch/");
  auto runDuration = cm.getRunDuration(runNumber);
  auto tRun = runDuration.first + (runDuration.second - runDuration.first) / 2; // time stamp for the middle of the run duration
  LOGP(info, "Timestamp: {}", tRun);
  cm.setTimestamp(tRun);

  // set the track topology correction
  o2::tpc::CalibdEdxTrackTopologyPolContainer* calibTrackTopologyContainer = cm.getForTimeStamp<o2::tpc::CalibdEdxTrackTopologyPolContainer>(o2::tpc::CDBTypeMap.at(o2::tpc::CDBType::CalTopologyGain), tRun);
  o2::tpc::CalibdEdxTrackTopologyPol calibTrackTopology;
  calibTrackTopology.setFromContainer(*calibTrackTopologyContainer);
  mCalibCont.setPolTopologyCorrection(calibTrackTopology);

  // set the gain map
  o2::tpc::CalDet<float>* gainMap = cm.getForTimeStamp<o2::tpc::CalDet<float>>(o2::tpc::CDBTypeMap.at(o2::tpc::CDBType::CalPadGainFull), tRun);
  const o2::tpc::CalDet<float> gainMapResidual = (*cm.getForTimeStamp<std::unordered_map<std::string, o2::tpc::CalDet<float>>>(o2::tpc::CDBTypeMap.at(o2::tpc::CDBType::CalPadGainResidual), tRun))["GainMap"];

  const float minGain = 0;
  const float maxGain = 2;
  mCalibCont.setGainMap(*gainMap, minGain, maxGain);
  mCalibCont.setGainMapResidual(gainMapResidual);

  // set the residual dEdx correction
  o2::tpc::CalibdEdxCorrection* residualObj = cm.getForTimeStamp<o2::tpc::CalibdEdxCorrection>(o2::tpc::CDBTypeMap.at(o2::tpc::CDBType::CalTimeGain), tRun);

  const auto* residualCorr = static_cast<o2::tpc::CalibdEdxCorrection*>(residualObj);
  mCalibCont.setResidualCorrection(*residualCorr);

  // set the zero supression threshold map
  std::unordered_map<string, o2::tpc::CalDet<float>>* zeroSupressionThresholdMap = cm.getForTimeStamp<std::unordered_map<string, o2::tpc::CalDet<float>>>(o2::tpc::CDBTypeMap.at(o2::tpc::CDBType::ConfigFEEPad), tRun);
  mCalibCont.setZeroSupresssionThreshold(zeroSupressionThresholdMap->at("ThresholdMap"));

  // set the magnetic field
  auto magField = cm.get<o2::parameters::GRPMagField>("GLO/Config/GRPMagField");
  o2::base::Propagator::initFieldFromGRP(magField);
  float bz = 5.00668f * magField->getL3Current() / 30000.;
  LOGP(info, "Magnetic field: {}", bz);
  setField(bz);

  // set the propagator
  auto propagator = o2::base::Propagator::Instance();
  const o2::base::MatLayerCylSet* matLut = o2::base::MatLayerCylSet::rectifyPtrFromFile(cm.get<o2::base::MatLayerCylSet>("GLO/Param/MatLUT"));
  propagator->setMatLUT(matLut);
}
