//===- odla_popart.cc -----------------------------------------------------===//
//
// Copyright (C) 2019-2020 Alibaba Group Holding Limited.
// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
#include <mutex>
#include <iostream>
#include <popart/builder.hpp>
#include <popart/dataflow.hpp>
#include "odla_popart.h"
#include "popart_config.h"
#include "odla_pipeline.h"

_odla_computation* _odla_computation::m_instance = new _odla_computation();

_odla_computation::_odla_computation():builder(popart::Builder::create()), 
    session(nullptr), device(nullptr), opts({false, 1, 1}), 
    m_done(false), m_executor(nullptr) 
{
    // Place All Subgraph on IPU 0, when no pipeline
    //if(PopartConfig::instance()->no_pipeline())
    //    builder->setAttribute(popart::sVirtualGraphAttribute, 0);
}

void _odla_computation::init()
{
    if(!session){
        std::lock_guard<std::mutex> guard(m_init_mutex);
        if(!session){
            set_opts(); //Test code
            //Cretate the dataflow
            std::vector<popart::TensorId> ids;
            for (const auto& output : outputs_map)
                ids.push_back(output.second->tensor_id);
            popart::DataFlow data_flow(opts.batches_per_step, ids,
                                    popart::AnchorReturnType("All"));
            // Acquire IPU
            if(opts.use_ipu_model){
                std::cout << "Using IPU Model to run " << std::endl;
                std::map<std::string, std::string> deviceOpts{
                    {"numIPUs", std::to_string(opts.ipu_num)}, {"tilesPerIPU", "1216"}};
                device = popart::DeviceManager::createDeviceManager().createIpuModelDevice(deviceOpts);
            }
            else
                device = popart::DeviceManager::createDeviceManager().acquireAvailableDevice(opts.ipu_num);
            // Create and config SessionOptions
            set_session_opts();

            auto proto = builder->getModelProto(); //So, the init must be called at odla_ExecuteCompute
            if(PopartConfig::instance()->load_onnx()){
                std::cout << "======> Load onnx file as pipeline mode to run: " << PopartConfig::instance()->load_onnx_path() << std::endl;
                proto = PopartConfig::instance()->load_onnx_path();
            }
            if(PopartConfig::instance()->save_model()){
                builder->saveModelProto(PopartConfig::instance()->save_model_path());
                std::cout << "The model saved to " << PopartConfig::instance()->save_model_path() << std::endl;
            }
            
            // Create InferenceSession
            session = popart::InferenceSession::createFromOnnxModel(
                proto,
                data_flow, 
                device, 
                popart::InputShapeInfo(), 
                m_session_opts
            );
            session->prepareDevice();
            session->setRandomSeed(0);  // Init seed
            session->weightsFromHost(); // Copy weights from host to IPU
        }
    }
}

// Now we set this by config file, should set by the caller?
void _odla_computation::set_opts()
{
    opts.use_ipu_model = false;
    opts.ipu_num = PopartConfig::instance()->ipu_num();
    opts.batches_per_step = PopartConfig::instance()->batch_per_step();
}

void _odla_computation::set_executor()
{
    ExecutionMode mode = PopartConfig::instance()->execution_mode();
    if(PIPELINE == mode || PARALLEL == mode){
        std::cout << "===============> set the executor as parallel" << std::endl;
        m_executor = new Parallel();
    }
    else if(SEQUENCE == mode){
        std::cout << "===============> set the executor as sequence" << std::endl;
        m_executor = new Sequence();
    }
    else{
        std::cerr << "*** FATAL *** unknown execution mode: " << mode << std::endl;
        exit(-1);
    }
}

void _odla_computation::set_session_opts()
{
    //This should be passed in by config file or some where
    ExecutionMode mode = PopartConfig::instance()->execution_mode();
    if(PIPELINE == mode){
        m_session_opts.enablePipelining = true;
        m_session_opts.autoRecomputation = popart::RecomputationType::Pipeline;
    }
    m_session_opts.matmulOptions["use128BitConvUnitLoad"] = "true";
    m_session_opts.matmulOptions["enableMultiStageReduce"] = "false";
    m_session_opts.matmulOptions["enableFastReduce"] = "true";
    m_session_opts.virtualGraphMode = popart::VirtualGraphMode::Manual;
    m_session_opts.enableFloatingPointChecks = false;
    m_session_opts.enableStochasticRounding = false;
    m_session_opts.enableGroupedMatmuls = false;
    m_session_opts.enablePrefetchDatastreams = true;
    m_session_opts.enableOutlining = true;
    std::string partials_type = "half";
    m_session_opts.partialsTypeMatMuls = partials_type;
    m_session_opts.convolutionOptions["partialsType"] = partials_type;
    m_session_opts.outlineThreshold = 10.0;
    m_session_opts.instrumentWithHardwareCycleCounter = false;
    m_session_opts.disableGradAccumulationTensorStreams = true;
}

void _odla_computation::set_pipeline_stage(const popart::TensorId &nodeOutputName, const std::string& name){
    if(PopartConfig::instance()->no_pipeline()){
        std::cout << "PIPELINE not used for this run " << std::endl;
        return;
    }
    std::cout << "Arranging the tenor with id: [" << nodeOutputName << "], name:[" << name << "]" << std::endl;
    int64_t ipu_idx = -1;
    int64_t pipeline_stage = -1;
    auto found = PopartConfig::instance()->get_pipeline_setting(name, ipu_idx, pipeline_stage);
    if(found){
      builder->virtualGraph(nodeOutputName, ipu_idx);
      builder->pipelineStage(nodeOutputName, pipeline_stage);
    }else{
      std::cerr << " *** FATAL *** did not find a setting for node: " << nodeOutputName 
                << ", name: " << name << " when do the pipeling" << std::endl;
      //exit(-1);
    }
}

void _odla_computation::set_pipeline_stage(const std::set<popart::TensorId> &nodeOutputNames, const std::string& name){
    if(PopartConfig::instance()->no_pipeline()){
        std::cout << "PIPELINE not used for this run " << std::endl;
        return;
    }
    std::cout << "Arranging the tenor with name:[" << name << "]" << std::endl;
    int64_t ipu_idx = -1;
    int64_t pipeline_stage = -1;
    auto found = PopartConfig::instance()->get_pipeline_setting(name, ipu_idx, pipeline_stage);
    if(found){
      builder->virtualGraph(nodeOutputNames, ipu_idx);
      builder->pipelineStage(nodeOutputNames, pipeline_stage);
    }else{
      std::cerr << " *** FATAL *** did not find a setting for name: " << name << " when do the pipeling" << std::endl;
      //exit(-1);
    }
}

void _odla_computation::set_pipeline_stage(const std::string& name)
{
    static bool global_ipu_number_set = false;
    if(PopartConfig::instance()->no_pipeline()){
        std::cout << "PIPELINE not used for this run " << std::endl;
        if(!global_ipu_number_set){
            std::cout << "Set the global virtual group to ipu 0" << std::endl;
            builder->setAttribute(popart::sVirtualGraphAttribute, 0);
        }
        return;
    }
    // Use local static to record whether the pipeline_stage_setting changed
    static int64_t previous_pipeline_stage_setting = -1;
    auto found = PopartConfig::instance()->get_pipeline_setting(name, m_ipu_number, m_pipeline_stage);
    if(found)
    {
        std::cout << "Found the pipeline setting change point with name: " << name 
                    << ", for which and following with setting __ipu_number: " << m_ipu_number
                    << ", __pipeline_stage: " << m_pipeline_stage << std::endl;
    }
    if(previous_pipeline_stage_setting != m_pipeline_stage)
    {
        std::cout << "pipeling setting will be: __ipu_number: " << m_ipu_number
                      << ", __pipeline_stage: " << m_pipeline_stage 
                      << ", from the node with name: " << name << std::endl;

        if(builder->hasAttribute(popart::sVirtualGraphAttribute))
            builder->clearAttribute(popart::sVirtualGraphAttribute);
        if(builder->hasAttribute(popart::sPipelineStageAttribute))
            builder->clearAttribute(popart::sPipelineStageAttribute);
        
        builder->setAttribute(popart::sVirtualGraphAttribute, m_ipu_number);
        builder->setAttribute(popart::sPipelineStageAttribute, m_pipeline_stage);
        previous_pipeline_stage_setting = m_pipeline_stage;
    }
}

void Sequence::compute(odla_computation comp, odla_context context,
                                odla_compute_mode mode, odla_device device) 
{
    comp->init();
    std::lock_guard<std::mutex> comp_guard(sequence_mutex);
    std::cout << "---> Sequence::compute()" << std::endl;
    // Config StepIO
    std::map<popart::TensorId, popart::IArray&> inputs;
    for (auto& input : context->inputs) {
        inputs.emplace(input.first, *input.second);
    }
    std::map<popart::TensorId, popart::IArray&> outputs;
    for (auto& output : context->outputs) {
        outputs.emplace(output.first, *output.second);
    }

    popart::StepIO stepio(inputs, outputs);
    // Run on ipu
    comp->session->run(stepio);
    std::cout << "<--- Sequence::compute()" << std::endl;
}

void Parallel::compute(odla_computation comp, odla_context context,
                       odla_compute_mode mode,odla_device device) 
{
    std::cout << "---> Parallel::compute()" << std::endl;
    ContextQueues::get_instance()->put(context);
    context->wait();
    std::cout << "<--- Parallel::compute()" << std::endl;
}

_odla_value::_odla_value(popart::TensorId id, popart::TensorInfo info,
    const std::string& n, bool set_pipeline /* = true */): tensor_id(id), tensor_info(info), name(n) 
{
    if(set_pipeline)
        g_comp->set_pipeline_stage(id, name);
    else
        std::cout << "The tensor with id: " << id << " should be solved some where previously." << std::endl; 
    //g_comp->set_pipeline_stage(name);
}