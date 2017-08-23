/* Copyright (c) 2017 PaddlePaddle Authors. All Rights Reserve.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "MKLDNNFcLayer.h"
#include "paddle/utils/Logging.h"
#include "paddle/utils/Stat.h"

using namespace mkldnn;  // NOLINT
typedef memory::format format;
typedef inner_product_forward fc_fwd;
typedef inner_product_backward_weights fc_bwdWgt;
typedef inner_product_backward_data fc_bwdData;

namespace paddle {

REGISTER_LAYER(mkldnn_fc, MKLDNNFcLayer);

bool MKLDNNFcLayer::init(const LayerMap& layerMap,
                         const ParameterMap& parameterMap) {
  if (!MKLDNNLayer::init(layerMap, parameterMap)) {
    return false;
  }

  CHECK_EQ(inputLayers_.size(), 1) << "Only support one input layer yet";
  CHECK_EQ(inputLayers_.size(), parameters_.size());
  CHECK(!parameters_[0]->isSparse()) << "Do not support sparse yet";

  // output size, cat not be changed
  oc_ = getSize();
  oh_ = 1;
  ow_ = 1;

  // input size can not change in FC
  iLayerSize_ = inputLayers_[0]->getSize();
  CHECK_EQ(parameters_[0]->getSize(), iLayerSize_ * oc_);

  // create weight
  weight_ =
      std::unique_ptr<Weight>(new Weight(oc_, iLayerSize_, parameters_[0], 0));

  // create biases
  if (biasParameter_.get() != NULL) {
    biases_ = std::unique_ptr<Weight>(new Weight(1, oc_, biasParameter_));
  }
  return true;
}

void MKLDNNFcLayer::convertWeightsFromPaddle() {
  if (hasInitedWgt_) {
    return;
  }

  // TODO(TJ): dst format should get from wgtVal_
  int dstFmt = PARAM_FORMAT_MKLDNN_OI;
  int srcFmt = weight_->getParameterPtr()->getHeaderFormat();
  if (srcFmt == dstFmt) {
    return;
  }

  // The weight_ is transposed from initial paddle weight
  MatrixPtr paddleWgt = Matrix::create(
      weight_->getW()->getData(), iLayerSize_, oc_, false, false);

  // TODO(TJ): remove this print when do not need differ weights
  std::ostringstream ostr;
  paddleWgt->print(ostr);
  VLOG(MKLDNN_ALL) << "Initial Weight from paddle: " << std::endl << ostr.str();

  // The mkldnn weight is transposed from initial paddle matrix
  MatrixPtr paddleWgtT;
  paddleWgt->transpose(paddleWgtT, true);
  weight_->getW()->copyFrom(*paddleWgtT);
  weight_->getParameterPtr()->setHeaderFormat(dstFmt);
  hasInitedWgt_ = true;
}

void MKLDNNFcLayer::convertWeightsToPaddle() {
  MatrixPtr dnnWgt = weight_->getW();
  MatrixPtr paddleWgt;
  dnnWgt->transpose(paddleWgt, true);

  // copy paddle weight and override on weight_
  MatrixPtr dnnWgtT = Matrix::create(
      dnnWgt->getData(), dnnWgt->getWidth(), dnnWgt->getHeight(), false, false);
  dnnWgtT->copyFrom(*paddleWgt);
}

void MKLDNNFcLayer::reshape() {
  const Argument& input = getInput(0, getPrev(0)->getDeviceId());
  int batchSize = input.getBatchSize();
  if (bs_ == batchSize) {
    return;
  }
  bs_ = batchSize;
  ih_ = input.getFrameHeight();
  iw_ = input.getFrameWidth();
  if (ih_ == 0) {
    ih_ = 1;
  }
  if (iw_ == 0) {
    iw_ = 1;
  }
  CHECK_EQ(iLayerSize_, inputLayers_[0]->getSize());
  ic_ = iLayerSize_ / (ih_ * iw_);
  CHECK_EQ(size_t(ic_ * ih_ * iw_), iLayerSize_) << "not divisible";
  CHECK_EQ(size_t(oc_), getSize());
  printSizeInfo();

  // reset output
  output_.setFrameHeight(oh_);
  output_.setFrameWidth(ow_);
  resetOutput(bs_, oc_);

  // reset mkldnn forward
  resetFwd();
  needResetBwd_ = true;

  convertWeightsFromPaddle();
}

void MKLDNNFcLayer::resetFwd() {
  bool hasBias = biases_ && biases_->getW();
  const MatrixPtr& wgt = weight_->getW();
  const MatrixPtr& bias = hasBias ? biases_->getW() : nullptr;
  const MatrixPtr& out = output_.value;

  if (prevIsMKLDNN()) {
    const MatrixPtr& in = getInputValue(0);
    inVal_ = std::dynamic_pointer_cast<MKLDNNMatrix>(in);
    CHECK(inVal_) << "Input should be MKLDNNMatrix";
  } else {
    CHECK_EQ(getPrev(0)->getDeviceId(), CPU_DEVICE) << "Only support CPU yet";
    const MatrixPtr& in = getInputValue(0, CPU_DEVICE);
    inVal_ = MKLDNNMatrix::create(
        in, memory::dims{bs_, ic_, ih_, iw_}, format::nchw, engine_);
  }
  inVal_->downSpatial();
  wgtVal_ = MKLDNNMatrix::create(
      wgt, memory::dims{oc_, ic_, ih_, iw_}, format::oihw, engine_);
  wgtVal_->downSpatial();
  biasVal_ =
      hasBias ? MKLDNNMatrix::create(bias, {oc_}, format::x, engine_) : nullptr;
  outVal_ = MKLDNNMatrix::create(out, {bs_, oc_}, format::nc, engine_);

  // change original output value to mkldnn output value
  output_.value = std::dynamic_pointer_cast<Matrix>(outVal_);
  if (!nextIsMKLDNN()) {
    Argument cpuOutput;
    for (size_t i = 0; i < outputOtherDevice_.size(); i++) {
      if (outputOtherDevice_[i].deviceId == CPU_DEVICE) {
        cpuOutput = outputOtherDevice_[i];
      }
    }
    cpuOutput.setFrameHeight(output_.getFrameHeight());
    cpuOutput.setFrameWidth(output_.getFrameWidth());

    // fc cpu output value do not need convert
    cpuOutput.value = output_.value;
  }

  // create forward handle
  prop_kind pk = prop_kind::forward;
  fc_fwd::desc fwdDesc =
      hasBias ? fc_fwd::desc(pk,
                             inVal_->getMD(),
                             wgtVal_->getMD(),
                             biasVal_->getMD(),
                             outVal_->getMD())
              : fc_fwd::desc(
                    pk, inVal_->getMD(), wgtVal_->getMD(), outVal_->getMD());
  fc_fwd::primitive_desc fwdPD = fc_fwd::primitive_desc(fwdDesc, engine_);
  if (hasBias) {
    fwd_.reset(new fc_fwd(fwdPD, *inVal_, *wgtVal_, *biasVal_, *outVal_));
  } else {
    fwd_.reset(new fc_fwd(fwdPD, *inVal_, *wgtVal_, *outVal_));
  }
  printValueFormatFlow();

  pipelineFwd_.clear();
  pipelineFwd_.push_back(*fwd_);
}

void MKLDNNFcLayer::resetBwd() {
  if (!needResetBwd_) {
    return;
  }
  needResetBwd_ = false;
  bool hasBias = biases_ && biases_->getWGrad();

  /// backward weight
  CHECK(inVal_) << "Should have input value";
  const MatrixPtr& wgt = weight_->getWGrad();
  const MatrixPtr& bias = hasBias ? biases_->getWGrad() : nullptr;

  if (nextIsMKLDNN()) {
    // can not directly cast outputgrad to mkldnnmatrix,
    // since each layer can not write the inputgrad to mkldnn inputgrad.
    // So just create from matrix with outputvalue format.
    const MatrixPtr& out = getOutput(MKLDNN_DEVICE).grad;
    outGrad_ = MKLDNNMatrix::create(out, outVal_->getPD());
    // TODO: maybe need merge topdiffs
  } else {
    // TODO: merge topdiffs
    const MatrixPtr& out = getOutput(CPU_DEVICE).grad;
    // fc do not need to convert from cpu device since output always nc
    // only need create from cpu device
    outGrad_ = MKLDNNMatrix::create(out, outVal_->getPD());
  }

  wgtGrad_ = MKLDNNMatrix::create(wgt, wgtVal_->getPD());
  biasGrad_ = hasBias ? MKLDNNMatrix::create(bias, biasVal_->getPD()) : nullptr;

  // create memory primitive desc
  fc_fwd::desc fwdDesc = fc_fwd::desc(prop_kind::forward,
                                      inVal_->getMD(),
                                      wgtGrad_->getMD(),
                                      outGrad_->getMD());
  fc_fwd::primitive_desc fwdPD = fc_fwd::primitive_desc(fwdDesc, engine_);
  fc_bwdWgt::desc bwdWgtDesc =
      hasBias ? fc_bwdWgt::desc(inVal_->getMD(),
                                wgtGrad_->getMD(),
                                biasGrad_->getMD(),
                                outGrad_->getMD())
              : fc_bwdWgt::desc(
                    inVal_->getMD(), wgtGrad_->getMD(), outGrad_->getMD());
  fc_bwdWgt::primitive_desc bwdWgtPD =
      fc_bwdWgt::primitive_desc(bwdWgtDesc, engine_, fwdPD);

  if (hasBias) {
    bwdWgt_.reset(
        new fc_bwdWgt(bwdWgtPD, *inVal_, *outGrad_, *wgtGrad_, *biasGrad_));
  } else {
    bwdWgt_.reset(new fc_bwdWgt(bwdWgtPD, *inVal_, *outGrad_, *wgtGrad_));
  }
  pipelineBwd_.clear();
  pipelineBwd_.push_back(*bwdWgt_);

  /// backward data
  if (prevIsMKLDNN()) {
    const MatrixPtr& in = getInputGrad(0, MKLDNN_DEVICE);
    if (in == nullptr) {
      return;
    }
    if (getInput(0, MKLDNN_DEVICE).getAllCount() > 1) {
      // TODO: many mkldnn bots
      // add sum handle
    } else {
      inGrad_ = MKLDNNMatrix::create(in, inVal_->getPD());
    }
  } else {
    const MatrixPtr& in = getInputGrad(0, CPU_DEVICE);
    if (in == nullptr) {
      return;
    }
    if (getInput(0, CPU_DEVICE).getAllCount() > 1) {
      // TODO: many  bots
      // add sum handle
    } else {
      inGrad_ = MKLDNNMatrix::create(in, inVal_->getPD());
    }
  }

  fc_bwdData::desc bwdDataDesc =
      fc_bwdData::desc(inVal_->getMD(), wgtGrad_->getMD(), outGrad_->getMD());
  fc_bwdData::primitive_desc bwdDataPD =
      fc_bwdData::primitive_desc(bwdDataDesc, engine_, fwdPD);

  CHECK(wgtVal_) << "Should have weight memory";
  bwdData_.reset(new fc_bwdData(bwdDataPD, *outGrad_, *wgtVal_, *inGrad_));
  printGradFormatFlow();
  pipelineBwd_.push_back(*bwdData_);
}

void MKLDNNFcLayer::forward(PassType passType) {
  Layer::forward(passType);
  reshape();

  {
    REGISTER_TIMER_INFO("mkldnn_FwdTimer", getName().c_str());
    syncInputValue();

    // just submit forward pipeline
    stream_->submit(pipelineFwd_);
  }

  /* activation */ {
    REGISTER_TIMER_INFO("FwActTimer", getName().c_str());
    forwardActivation();
  }
}

void MKLDNNFcLayer::backward(const UpdateCallback& callback) {
  /* Do derivation */ {
    REGISTER_TIMER_INFO("BpActTimer", getName().c_str());
    backwardActivation();
  }

  {
    REGISTER_TIMER_INFO("mkldnn_bwdTimer", getName().c_str());
    resetBwd();

    syncOutputGrad();
    // just sumbmit backward pipeline
    stream_->submit(pipelineBwd_);
  }

  {
    REGISTER_TIMER_INFO("WeightUpdate", getName().c_str());
    weight_->getParameterPtr()->incUpdate(callback);
    if (biases_ && biases_->getWGrad()) {
      biases_->getParameterPtr()->incUpdate(callback);
    }
  }
}
}  // namespace paddle
