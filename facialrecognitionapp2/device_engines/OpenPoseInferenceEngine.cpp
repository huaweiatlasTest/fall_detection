/**
*
* Copyright(c)<2018>, <Huawei Technologies Co.,Ltd>
*
* @version 1.0
*
* @date 2018-5-22
*/
#include "OpenPoseInferenceEngine.h"
#include <hiaiengine/log.h>
#include <hiaiengine/ai_types.h>
#include "hiaiengine/ai_memory.h"
#include <vector>
#include <unistd.h>
#include <thread>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <time.h>
const static int IMAGE_INFO_DATA_NUM = 3;
const static int16_t DEFAULT_MEAN_VALUE_CHANNEL_0 = 104;
const static int16_t DEFAULT_MEAN_VALUE_CHANNEL_1 = 117;
const static int16_t DEFAULT_MEAN_VALUE_CHANNEL_2 = 123;
const static int16_t DEFAULT_MEAN_VALUE_CHANNEL_3 = 0;
const static float DEFAULT_RECI_VALUE = 1.0;
const static int32_t CROP_START_LOCATION = 0;
const static int32_t WIDTH_ALIGN = 128;
const static int32_t HEIGHT_ALIGN = 16;
const static int32_t BLANK_SET_NUMBER = 0;
const static std::string DYNAMIC_AIPP = "1";
const static std::string STATIC_AIPP = "0";
const static std::string MODEL_AIPP_TYPE_NOT_SET = "-1";
const static uint32_t INPUT_EDGE_INDEX_0 = 0;
const static uint32_t INPUT_INDEX_0 = 0;

/**
* @brief: clear buffer in vector
*/
void OpenPoseInferenceEngine::ClearOutData()
{
    input_data_vec.clear();
    // release outData pre allocate memmory
    for (auto buffer : m_outData_) {
        if (buffer != nullptr) {
            hiai::HIAIMemory::HIAI_DFree(buffer);
            buffer = nullptr;
        }
    }
    m_outData_.clear();
}

/**
* @brief: init, inherited from hiaiengine lib
*/
HIAI_StatusT OpenPoseInferenceEngine::Init(const hiai::AIConfig& config,
   const  std::vector<hiai::AIModelDescription>& model_desc)
{
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] start init!");
    hiai::AIStatus ret = hiai::SUCCESS;

    if (nullptr == ai_model_manager_)
    {
        ai_model_manager_ = std::make_shared<hiai::AIModelManager>();
    }

    std::vector<hiai::AIModelDescription> model_desc_vec;
    hiai::AIModelDescription model_desc_;
    for (int index = 0; index < config.items_size(); ++index)
    {
        const ::hiai::AIConfigItem& item = config.items(index);
        if(item.name() == "model_path")
        {
            std::string model_path = item.value();
            if(model_path.empty()){
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] model_path not exist!");
                return HIAI_ERROR;
            }
            model_desc_.set_path(model_path);
            std::size_t modelNameStartPos = model_path.find_last_of("/\\");
            std::size_t modelNameEndPos = model_path.find_last_of(".");
            if (std::string::npos != modelNameStartPos && std::string::npos != modelNameEndPos
                && modelNameEndPos > modelNameStartPos) {
                modelName_ = model_path.substr(modelNameStartPos + 1, modelNameEndPos - modelNameStartPos - 1);
            }
        } else if (item.name() == "passcode") {
            std::string passcode = item.value();
            model_desc_.set_key(passcode);
        } else if (item.name() == "input_image_width") {
            if (item.value() == "") {
                input_image_width = BLANK_SET_NUMBER;
                crop_width = BLANK_SET_NUMBER;
            } else {
                std::stringstream ss(item.value());
                ss >> input_image_width;
                if(input_image_width <= 0){
                    HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] input_image_width <= 0!");
                    return HIAI_ERROR;
                }
                crop_width = input_image_width;
                input_image_width = ceil(input_image_width * 1.0 / WIDTH_ALIGN) * WIDTH_ALIGN;
            }
            
        } else if (item.name() == "input_image_height") {
            if (item.value() == "") {
                input_image_height = BLANK_SET_NUMBER;
                crop_height = BLANK_SET_NUMBER;
            } else {
                std::stringstream ss(item.value());
                ss >> input_image_height;
                if(input_image_height <= 0){
                    HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] input_image_height <= 0!");
                    return HIAI_ERROR;
                }
                crop_height = input_image_height;
                input_image_height = ceil(input_image_height * 1.0 / HEIGHT_ALIGN) * HEIGHT_ALIGN;
            }
        }
    }
    model_desc_.set_name(modelName_);
    model_desc_vec.push_back(model_desc_);
    ret = ai_model_manager_->Init(config, model_desc_vec);
    if (hiai::SUCCESS != ret)
    {
        return HIAI_ERROR;
    }

    ret = ai_model_manager_->GetModelIOTensorDim(modelName_, inputTensorVec, outputTensorVec);
    if (ret != hiai::SUCCESS)    {
        HIAI_ENGINE_LOG(this, HIAI_IDE_ERROR, "hiai ai model manager init fail");
        return HIAI_ERROR;
    }
    batch_size = inputTensorVec[INPUT_INDEX_0].n;
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] end init!");
    return HIAI_OK;
}

/**
* @brief: handle the exceptions when the dataset batch failed
* @in: error_msg: the error message
*/
void OpenPoseInferenceEngine::HandleExceptions(std::string error_msg) {
    HIAI_ENGINE_LOG(HIAI_IDE_ERROR, error_msg.c_str());
    tran_data->status = false;
    tran_data->msg = error_msg;
    //send null to next node to avoid blocking when to encounter abnomal situation.
    auto ret = SendData(0, "EngineTransT", std::static_pointer_cast<void>(tran_data));
    if (ret != HIAI_OK) {
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] send data failed!");
    }
};

/**
* @brief: send sentinel image to inform the graph to destroy
*/
HIAI_StatusT OpenPoseInferenceEngine::SendSentinelImage() {
    tran_data->status = true;
    tran_data->msg = "sentinel Image";
    tran_data->b_info = image_handle->b_info;
    HIAI_StatusT hiai_ret = HIAI_OK;
    do {
        HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] sentinel image, process success!");
        hiai_ret = SendData(0, "EngineTransT", std::static_pointer_cast<void>(tran_data));
        hiai_ret = SendData(1, "EngineTransT", std::static_pointer_cast<void>(tran_data));
        hiai_ret = SendData(2, "EngineTransT", std::static_pointer_cast<void>(tran_data));
        hiai_ret = SendData(3, "EngineTransT", std::static_pointer_cast<void>(tran_data));
        if (HIAI_OK != hiai_ret) {
            if (HIAI_ENGINE_NULL_POINTER == hiai_ret || HIAI_HDC_SEND_MSG_ERROR == hiai_ret || HIAI_HDC_SEND_ERROR == hiai_ret
                || HIAI_GRAPH_SRC_PORT_NOT_EXIST == hiai_ret || HIAI_GRAPH_ENGINE_NOT_EXIST == hiai_ret) {
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] SendData error[%d], break.", hiai_ret);
                break;
            }
            HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] SendData return value[%d] not OK, sleep 200ms", hiai_ret);
            usleep(SEND_DATA_INTERVAL_MS);
        }
    } while (HIAI_OK != hiai_ret);
    return hiai_ret;
}

/**
* @brief: prepare the data buffer for image information
* @in: input_buffer: buffer pointer
* @in: image_number: total number of received images
* @in: batch_begin: the index of the first image of each batch
* @in: image_size: size of each image
* @return: HIAI_StatusT
*/
HIAI_StatusT OpenPoseInferenceEngine::PrepareInputBuffer(uint8_t *input_buffer,const int image_number, const int batch_begin, const int image_size) {
    //1.prepare input buffer for each batch
    //the loop for each image
    if(input_buffer ==  nullptr){
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] ERROR, input_buffer is nullptr");
        return HIAI_ERROR;
    }
    for (int j = 0; j < int(batch_size); j++) {
        if (batch_begin + j < image_number) {
            if (memcpy_s(input_buffer + j*image_size, image_size, image_handle->v_img[batch_begin + j].img.data.get(), image_size)) {
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] ERROR, copy image buffer failed");
                return HIAI_ERROR;
            }
        }
        else {
            if (memset_s(input_buffer + j*image_size, image_size, static_cast<char>(0), image_size)) {
                HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] ERROR, batch padding for image data failed");
                return HIAI_ERROR;
            }
        }
    }
    return HIAI_OK;
}

/**
* @brief: call ai model manager to do the prediction
* @return: HIAI_StatusT
*/
HIAI_StatusT OpenPoseInferenceEngine::Predict() {
    //pre malloc OutData
    HIAI_StatusT hiai_ret = HIAI_OK;
    for (uint32_t index = 0; index < outputTensorVec.size(); index++) {
        hiai::AITensorDescription outputTensorDesc = hiai::AINeuralNetworkBuffer::GetDescription();
        uint8_t* buf = nullptr;
        hiai_ret = hiai::HIAIMemory::HIAI_DMalloc(outputTensorVec[index].size, (void *&)buf, 1000);
        if (hiai_ret != HIAI_OK || buf == nullptr) {
            HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] HIAI_DMalloc failed.");
            ClearOutData();
            return HIAI_ERROR;
        }
        m_outData_.push_back(buf);
        std::shared_ptr<hiai::IAITensor> outputTensor = hiai::AITensorFactory::GetInstance()->CreateTensor(outputTensorDesc, buf, outputTensorVec[index].size);
        shared_ptr<hiai::AINeuralNetworkBuffer> nn_tensor = static_pointer_cast<hiai::AINeuralNetworkBuffer>(outputTensor);
        nn_tensor->SetName(outputTensorVec[index].name);
        output_data_vec.push_back(outputTensor);
    }

    // put buffer to FrameWork directly, InputSize has only one
    hiai::AITensorDescription inputTensorDesc = hiai::AINeuralNetworkBuffer::GetDescription();
    for (unsigned int i = 0; i < predict_input_data_.size(); i++) {
        std::map<uint8_t *, int> tmp = predict_input_data_[i];
        for (std::map<uint8_t *, int>::iterator it = tmp.begin();it != tmp.end(); ++it) {
            shared_ptr<hiai::IAITensor> inputTensor =
                hiai::AITensorFactory::GetInstance()->CreateTensor(inputTensorDesc, (void *)(it->first), it->second);
            input_data_vec.push_back(inputTensor); // AIModelManager push input data
        }
    }

    HIAI_StatusT ret = HIAI_OK;
    
    hiai::AIContext ai_context;
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] ai_model_manager_->Process start!");
    ret = ai_model_manager_->Process(ai_context, input_data_vec, output_data_vec, 0);
    if (hiai::SUCCESS != ret)
    {
        ClearOutData();
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] ai_model_manager Process failed");
        return HIAI_ERROR;
    }
    return HIAI_OK;
}

/**
* @brief: set the tran_data with the result of this batch
* @in: index of the begin of this batch
* @return: HIAI_StatusT
*/
HIAI_StatusT OpenPoseInferenceEngine::SetOutputStruct(const int batch_begin) {
    for (unsigned int i = 0; i < output_data_vec.size(); ++i)
    {
        HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] build: %d", i);
        std::shared_ptr<hiai::AINeuralNetworkBuffer> result_tensor = std::static_pointer_cast<hiai::AINeuralNetworkBuffer>(output_data_vec[i]);
        auto tensor_size = result_tensor->GetSize();
        if (memcpy_s(tran_data->output_data_vec[i].data.get() + batch_begin/batch_size*tensor_size, tensor_size, result_tensor->GetBuffer(), tensor_size)) {
            return HIAI_ERROR;
        }
        HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] build: %d, number: %d", tensor_size, batch_begin / batch_size*tensor_size);
    }
    return HIAI_OK;
}

/**
* @brief: send the predicted result for one batch
*/
void OpenPoseInferenceEngine::SendResult() {
    HIAI_StatusT hiai_ret = HIAI_OK;
    for(int i = 0; i < 4; i++){
        std::shared_ptr<EngineTransT> tran_data1 = std::make_shared<EngineTransT>();
        tran_data1->b_info = tran_data->b_info;
        tran_data1->v_img = tran_data->v_img;
        tran_data1->status = true;
        OutputT heatmap = tran_data->output_data_vec[0];
        tran_data1->output_data_vec.push_back(heatmap);
        OutputT pafmap = tran_data->output_data_vec[1];
        int buffer_size;
        if(i == 3){
            buffer_size = pafmap.size*5/26;
        }else{
            buffer_size = pafmap.size*7/26;
        }
        // shared_ptr<u_int8_t> send_data(new u_int8_t[buffer_size]);
        // memcpy_s(resized_data.get(), buffer_size, resizePafMat.data, buffer_size);
        shared_ptr<u_int8_t> send_data(new u_int8_t[buffer_size]);
        if(i == 3){
            memcpy_s(send_data.get(), buffer_size, pafmap.data.get() + pafmap.size*21/26, buffer_size);
            // cout << "send:" << std::to_string(i) << ":"<<*((float*)send_data.get() + 5) << endl;
        }else{
            memcpy_s(send_data.get(), buffer_size, pafmap.data.get() + i * buffer_size, buffer_size);
            // cout << "send:" << std::to_string(i) << ":"<<*((float*)send_data.get() + 7) << endl;
        }
        OutputT out;
        out.size = buffer_size;
        out.data = send_data;
        out.name = "pafmaps";
        tran_data1->output_data_vec.push_back(out);
        tran_data1->msg = std::to_string(i);
        hiai_ret = SendData(i, "EngineTransT", std::static_pointer_cast<void>(tran_data1));
    }
        
    if (HIAI_OK != hiai_ret)
    {
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] SendData failed! error code: %d", hiai_ret);
    }
}

/**
* @brief: set the frame ID as -1 to indicate this model batch failed
* @in: index of the begin of this batch
*/
void OpenPoseInferenceEngine::HandleModelBatchFailure(const int batch_begin, const int image_number) {
    for (int i = 0; i < int(batch_size); i++) {
        if(batch_begin + i < image_number){
            tran_data->b_info.frame_ID[i + batch_begin] = -1;
        }
    }
}

/**
* @ingroup hiaiengine
* @brief HIAI_DEFINE_PROCESS : Realize the port input/output processing
* @[in]: Define an input port, an output port,
*        And the Engine is registered, its called "HIAIMultiEngineExample"
*/
HIAI_IMPL_ENGINE_PROCESS("OpenPoseInferenceEngine", OpenPoseInferenceEngine, INPUT_SIZE)
{
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] start process!");
    HIAI_StatusT hiai_ret = HIAI_OK;
    std::lock_guard<std::mutex> lk(memoryRecursiveMutex_);
    if (tran_data == nullptr) {
        tran_data = std::make_shared<EngineTransT>();
    }
    // 1.PreProcess:Framework input data
    if (nullptr != arg0)
    {
        std::shared_ptr<BatchImageParaWithScaleT> dataInput = std::static_pointer_cast<BatchImageParaWithScaleT>(arg0);

        if (!isSentinelImage(dataInput)) {
            if (dataInputIn_ != nullptr) {
                if (dataInputIn_->b_info.batch_ID == dataInput->b_info.batch_ID && !dataInput->v_img.empty() && !dataInput->b_info.frame_ID.empty()) {
                    dataInputIn_->v_img.push_back(dataInput->v_img[0]);
                    dataInputIn_->b_info.frame_ID.push_back(dataInput->b_info.frame_ID[0]);
                }
            } else {
                dataInputIn_ = std::make_shared<BatchImageParaWithScaleT>();
                if (dataInputIn_ == nullptr){
                    HIAI_ENGINE_LOG(HIAI_IDE_WARNING, "[OpenPoseInferenceEngine] malloc error");
                    return HIAI_ERROR;
                }
                for (int i = 0; i < int(dataInput->b_info.frame_ID.size()); ++i){
                    dataInputIn_->b_info.frame_ID.push_back(dataInput->b_info.frame_ID[i]);
                }
                dataInputIn_->b_info.batch_size = dataInput->b_info.batch_size;
                dataInputIn_->b_info.max_batch_size = dataInput->b_info.max_batch_size;
                dataInputIn_->b_info.batch_ID = dataInput->b_info.batch_ID;
                dataInputIn_->b_info.is_first = dataInput->b_info.is_first;
                dataInputIn_->b_info.is_last = dataInput->b_info.is_last;
                for(int i = 0; i < int(dataInput->v_img.size()); ++i){
                    dataInputIn_->v_img.push_back(dataInput->v_img[i]);
                }
            }
            if (dataInputIn_->v_img.size() != dataInputIn_->b_info.batch_size) {
                HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] Wait for other %d batch image info!", (dataInputIn_->b_info.batch_size - dataInputIn_->v_img.size()));
                return HIAI_OK;
            }
            input_que_.PushData(0, dataInputIn_);
            dataInputIn_ = nullptr;
        } else {
            input_que_.PushData(0, arg0);
        }
    }

    image_handle = nullptr;

    if (!input_que_.PopAllData(image_handle)) \
    {
        HandleExceptions("[OpenPoseInferenceEngine] fail to PopAllData");
        return HIAI_ERROR;
    }

    if (nullptr == image_handle)
    {
        HandleExceptions("[OpenPoseInferenceEngine] Image_handle is nullptr");
        return HIAI_ERROR;
    }
    //add sentinel image for showing this data in dataset are all sended, this is last step.
    if (isSentinelImage(image_handle))
    {
        return SendSentinelImage();
    }

    int image_number = image_handle->v_img.size();
    int image_size = image_handle->v_img[0].img.size * sizeof(uint8_t);
    int _input_buffer1_size = image_size * batch_size;
    if(_input_buffer1_size <= 0){
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] _input_buffer1_size <= 0");
        return HIAI_ERROR;
    }
    uint8_t *_input_buffer1 = nullptr;
    hiai_ret = hiai::HIAIMemory::HIAI_DMalloc(_input_buffer1_size, (void *&)_input_buffer1, 1000);
    if (hiai_ret != HIAI_OK || _input_buffer1 == nullptr) {
        HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] HIAI_DMalloc _input_buffer1 failed.");
        return HIAI_ERROR;
    }

    int cnt_batch = image_handle->b_info.batch_size / batch_size;
    if (image_handle->b_info.batch_size % batch_size != 0) {
        cnt_batch ++;
    }

    tran_data->b_info = image_handle->b_info;
    tran_data->v_img = image_handle->v_img;
    tran_data->status = true;
    tran_data->b_info.max_batch_size = cnt_batch* batch_size;
    //the loop for each batch
    for (int i = 0; i < image_number; i += batch_size) {
        predict_input_data_.clear();
        //1.prepare input buffer for each batch
        if (HIAI_OK != PrepareInputBuffer(_input_buffer1, image_number, i, image_size)) {
            HandleModelBatchFailure(i, image_number);
            continue;
        }
        std::map<uint8_t *, int> input1;
        input1.insert(std::make_pair(_input_buffer1, _input_buffer1_size));
        predict_input_data_.push_back(input1);

        DEFINE_MULTI_INPUT_ARGS(2);

        // 2.Call Process, Predict
        input_data_vec.clear();
        if (HIAI_OK != Predict()) {
            output_data_vec.clear();
            HandleModelBatchFailure(i, image_number);
            continue;
        }
        //init the output buffer for one dataset batch(might be multiple model batches)
        if (tran_data->output_data_vec.empty()) {
            tran_data->size = output_data_vec.size();
            HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] alloc memory for dataset batch, number of outputs of the network: %d", output_data_vec.size());
            for (unsigned int i = 0; i < output_data_vec.size(); i++) {
                OutputT out;
                std::shared_ptr<hiai::AINeuralNetworkBuffer> result_tensor = std::static_pointer_cast<hiai::AINeuralNetworkBuffer>(output_data_vec[i]);
                int buffer_size = result_tensor->GetSize();
                out.name = result_tensor->GetName();
                out.size = buffer_size * cnt_batch;
                if(out.size <= 0){
                    HIAI_ENGINE_LOG(HIAI_IDE_ERROR, "[OpenPoseInferenceEngine] out.size <= 0");
                    hiai::HIAIMemory::HIAI_DFree(_input_buffer1);
                    _input_buffer1 = nullptr;
                    ClearOutData();
                    return HIAI_ERROR;
                }
                u_int8_t *ptr = nullptr;
                try {
                    ptr = new u_int8_t[out.size];
                }
                catch (const std::bad_alloc& e) {
                    hiai::HIAIMemory::HIAI_DFree(_input_buffer1);
                    _input_buffer1 = nullptr;
                    ClearOutData();
                    return HIAI_ERROR;
                }
                out.data.reset(ptr);
                tran_data->output_data_vec.push_back(out);
                HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] cnt_model_batch: %d, image number: %d!", cnt_batch, image_number);
            }
        }

        //3.set the tran_data with the result of this batch
        if (HIAI_OK != SetOutputStruct(i)) {
            ClearOutData();
            output_data_vec.clear();
            HandleModelBatchFailure(i, image_number);
            continue;
        }
        output_data_vec.clear();
    }

    SendResult();
    //6. release sources
    hiai::HIAIMemory::HIAI_DFree(_input_buffer1);
    _input_buffer1 = nullptr;
    ClearOutData();
    tran_data = nullptr;
    HIAI_ENGINE_LOG(HIAI_IDE_INFO, "[OpenPoseInferenceEngine] end process!");
    // cout << "[OpenPoseInferenceEngine] end process!" << endl;
    return HIAI_OK;
}
