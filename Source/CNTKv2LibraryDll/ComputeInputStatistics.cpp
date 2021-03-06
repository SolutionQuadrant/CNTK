//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include "CNTKLibrary.h"
#include "Utils.h"
#include "Function.h"
#include <tuple>
#include "ComputationNetworkBuilder.h"

using namespace Microsoft::MSR::CNTK;

namespace CNTK
{
    void ComputeInputPerDimMeansAndInvStdDevs(const MinibatchSourcePtr& minibatchSource,
                                              std::unordered_map<StreamInformation, std::pair<NDArrayViewPtr, NDArrayViewPtr>>& computedMeanAndInvStdDevs,
                                              const DeviceDescriptor& device /*= DeviceDescriptor::CPUDevice()*/)
    {
        typedef std::shared_ptr<ComputationNode<float>> ComputationNodePtr;
        const auto& minibatchSourceStreams = minibatchSource->StreamInfos();

        auto computationNetwork = std::make_shared<ComputationNetwork>(AsCNTKImplDeviceId(device));
        ComputationNetworkBuilder<float> builder(*computationNetwork);

        std::vector<ComputationNodeBasePtr> allInputNodes;
        std::unordered_map<StreamInformation, ComputationNodeBasePtr> streamToInputNodeMap;
        std::unordered_map<StreamInformation, Variable> streamToDummyInputVariableMap;
        std::unordered_map<StreamInformation, ComputationNodeBasePtr> streamToMeanNodeMap;
        std::unordered_map<StreamInformation, ComputationNodeBasePtr> streamToInvStdDevNodeMap;

        size_t totalSizePerSample = 0;
        for (auto& currentStreamKV : computedMeanAndInvStdDevs)
        {
            auto currentStreamInfo = currentStreamKV.first;
            if (minibatchSourceStreams.find(currentStreamInfo) == minibatchSourceStreams.end())
                InvalidArgument("ComputeMeanAndVariance: Stream for which mean and variance is to be computed is not supported by the specified minibatchSource");

            if (currentStreamInfo.m_elementType != DataType::Float)
                LogicError("Input data of type other than DataType::Float is currently unsupported by the CNTK built-in composite MinibatchSource!");

            auto inputVariableShape = currentStreamInfo.m_sampleLayout;
            auto inputTensorShape = AsTensorShape(inputVariableShape);
            totalSizePerSample += (inputVariableShape.TotalSize() * sizeof(float));

            ComputationNodePtr inputNode;
            Variable inputVariable;
            if (currentStreamInfo.m_storageFormat != StorageFormat::Dense)
            {
                inputNode = builder.CreateSparseInputNode(currentStreamInfo.m_name, inputTensorShape);
                inputVariable = InputVariable(inputVariableShape, /*isSparse*/  true, DataType::Float, currentStreamInfo.m_name);
            }
            else
            {
                inputNode = builder.CreateInputNode(currentStreamInfo.m_name, inputTensorShape);
                inputVariable = InputVariable(inputVariableShape, DataType::Float, currentStreamInfo.m_name);
            }

            allInputNodes.push_back(inputNode);
            streamToInputNodeMap[currentStreamInfo] = inputNode;
            streamToDummyInputVariableMap[currentStreamInfo] = inputVariable;
            streamToMeanNodeMap[currentStreamInfo] = builder.Mean(inputNode);
            streamToInvStdDevNodeMap[currentStreamInfo] = builder.InvStdDev(inputNode);
        }

        computationNetwork->CompileNetwork();
        computationNetwork->AllocateAllMatrices(computationNetwork->RootNodes(), {}, nullptr);

        ScopedNetworkOperationMode modeGuard(computationNetwork, NetworkOperationMode::preComputing);

        // initialize
        auto preComputeNodes = computationNetwork->GetNodesRequiringPreComputation();
        for (auto & preComputeNode : preComputeNodes)
            dynamic_pointer_cast<IPreComputeNode>(preComputeNode)->MarkComputed(false /*begin accumulating*/);

        const size_t maxMinibatchDataSize = (1 << 27); // 128 MB
        const size_t minibatchSize = maxMinibatchDataSize / totalSizePerSample;
        for (;;)
        {
            auto minibatchData = minibatchSource->GetNextMinibatch(minibatchSize, device);
            if (minibatchData.empty())
                break;

            for (auto& currentStreamKV : computedMeanAndInvStdDevs)
                CompositeFunction::PopulateComputationNodeValue<float>({ streamToDummyInputVariableMap[currentStreamKV.first], minibatchData[currentStreamKV.first].m_data }, streamToInputNodeMap[currentStreamKV.first]);

            ComputationNetwork::BumpEvalTimeStamp(allInputNodes);

            computationNetwork->ForwardProp(preComputeNodes);
        }

        // finalize
        for (auto & preComputeNode : preComputeNodes)
            dynamic_pointer_cast<IPreComputeNode>(preComputeNode)->MarkComputed(true /*done accumulating*/);

        // Copy out the results
        for (auto& currentStreamKV : computedMeanAndInvStdDevs)
        {
            ValuePtr mean, invStdDev;
            if (computedMeanAndInvStdDevs[currentStreamKV.first].first != nullptr)
                mean = MakeSharedObject<Value>(computedMeanAndInvStdDevs[currentStreamKV.first].first);

            if (computedMeanAndInvStdDevs[currentStreamKV.first].second != nullptr)
                invStdDev = MakeSharedObject<Value>(computedMeanAndInvStdDevs[currentStreamKV.first].second);

            CompositeFunction::GetNodeOutputOrGradient(streamToDummyInputVariableMap[currentStreamKV.first], mean, streamToMeanNodeMap[currentStreamKV.first], false /*getGradient*/);
            CompositeFunction::GetNodeOutputOrGradient(streamToDummyInputVariableMap[currentStreamKV.first], invStdDev, streamToInvStdDevNodeMap[currentStreamKV.first], false /*getGradient*/);

            if (computedMeanAndInvStdDevs[currentStreamKV.first].first == nullptr)
                computedMeanAndInvStdDevs[currentStreamKV.first].first = mean->Data();

            if (computedMeanAndInvStdDevs[currentStreamKV.first].second == nullptr)
                computedMeanAndInvStdDevs[currentStreamKV.first].second = invStdDev->Data();
        }
    }
}
