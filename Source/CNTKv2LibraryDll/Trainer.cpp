//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include "CNTKLibrary.h"
#include "Utils.h"
#include "Function.h"

namespace CNTK
{
    Trainer::Trainer(const FunctionPtr& model, const FunctionPtr& lossFunction, const FunctionPtr& evaluationFunction, const std::unordered_set<LearnerPtr>& parameterLearners)
        : m_model(model), m_lossFunction(lossFunction), m_evaluationFunction(evaluationFunction), m_parameterLearners(parameterLearners), m_prevMinibatchNumSamples(1)
    {
        m_combinedTrainingFunction = Combine({ model, lossFunction, evaluationFunction });

        auto modelParameters = m_combinedTrainingFunction->Parameters();
        std::unordered_set<Parameter> learnerParameters;
        for (const auto& learner : parameterLearners)
        {
            const auto& currentLearnerParameters = learner->Parameters();
            for (const auto& parameter : currentLearnerParameters)
            {
                auto insertRetVal = learnerParameters.insert(parameter);
                if (!insertRetVal.second)
                    InvalidArgument("Trainer ctor: Parameter named %S is covered by 2 different learners", parameter.Name().c_str());
            }
        }

        if (modelParameters != learnerParameters)
            InvalidArgument("Trainer ctor: Union of the parameters covered by the specified parameterLearners should match the specified model's parameters");
    }

    Trainer::Trainer(const FunctionPtr& model, const FunctionPtr& lossFunction, const std::unordered_set<LearnerPtr>& parameterLearners)
        : Trainer(model, lossFunction, nullptr, parameterLearners)
    {}

    static double GetScalarValue(const ValuePtr& value)
    {
        if (value->Mask())
            LogicError("Scalar Value object cannot have an associated mask");

        auto scalarData = value->Data();
        if (scalarData->Shape().TotalSize() != 1)
            LogicError("Scalar Value object's has a size > 1");

        double scalar = std::numeric_limits<double>::quiet_NaN();
        NDArrayViewPtr cpuData;
        if (scalarData->Device() == DeviceDescriptor::CPUDevice())
            cpuData = scalarData;
        else
        {
            cpuData = std::make_shared<NDArrayView>(scalarData->GetDataType(), scalarData->Shape(), CNTK::DeviceDescriptor::CPUDevice());
            cpuData->CopyFrom(*scalarData);
        }

        if (scalarData->GetDataType() == DataType::Float)
            scalar = *(cpuData->DataBuffer<float>());
        else if (scalarData->GetDataType() == DataType::Double)
            scalar = *(cpuData->DataBuffer<double>());
        else
            LogicError("Unsupported DataType of training loss value");

        return scalar;
    }

    static size_t GetSampleCountFromArguments(const Variable& evalOrLossArgument, const std::unordered_map<Variable, ValuePtr>& arguments)
    {
        // Find the argument whose dynamic axes match the criterion operation's dynamic axes (i.e. label dynamic axes)
        // Then we determine the actual number of samples contributing to the training loss from the argument's Value object
        auto argumentIter = std::find_if(arguments.begin(), arguments.end(), [evalOrLossArgument](const std::pair<Variable, ValuePtr>& currentPair) {
            return (currentPair.first.DynamicAxes() == evalOrLossArgument.DynamicAxes());
        });

        auto argumentValue = argumentIter->second;
        auto argumentVar = argumentIter->first;
        auto argumentDataShape = argumentValue->Data()->Shape();
        auto mask = argumentValue->Mask();
        size_t numMaskedSamples = (mask != nullptr) ? mask->MaskedCount() : 0;
        size_t numSamplesInDataArrayView = argumentDataShape.SubShape(argumentVar.Shape().Rank()).TotalSize();
        if (numMaskedSamples > numSamplesInDataArrayView)
            LogicError("Number of masked values cannot exceed the number of samples that the Value object's Data NDArrayView can hold");

        return (numSamplesInDataArrayView - numMaskedSamples);
    }

    double Trainer::TestMinbatch(const std::unordered_map<Variable, ValuePtr>& arguments, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        if (!m_evaluationFunction)
            InvalidArgument("Trainer::TestMinbatch: Cannot test when no evaluation function was specified during 'this' trainer's construction");

        // TODO: Should we refactor this code that is somewhat similar to the prologue of the TrainMinibatch function
        std::unordered_map<Variable, ValuePtr> outputs = { { m_evaluationFunction, nullptr } };
        m_combinedTrainingFunction->Forward(arguments, outputs, computeDevice);

        auto sampleCount = GetSampleCountFromArguments(*(m_evaluationFunction->Arguments().begin()), arguments);
        return (GetScalarValue(outputs[m_evaluationFunction]) / sampleCount);
    }

    bool Trainer::TrainMinibatch(const std::unordered_map<Variable, ValuePtr>& arguments, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        std::unordered_map<Variable, ValuePtr> outputs = { { m_lossFunction, nullptr } };
        if (m_evaluationFunction)
            outputs.insert({ m_evaluationFunction, nullptr });

        auto backPropSate = m_combinedTrainingFunction->Forward(arguments, outputs, computeDevice, { m_lossFunction });
        m_prevMinibatchAggregateTrainingLossValue = outputs[m_lossFunction];
        if (m_evaluationFunction)
            m_prevMinibatchAggregateEvalCriterionValue = outputs[m_evaluationFunction];

        ValuePtr rootGradientValue = MakeSharedObject<Value>(MakeSharedObject<NDArrayView>(m_lossFunction->Output().GetDataType(), m_prevMinibatchAggregateTrainingLossValue->Data()->Shape(), computeDevice), outputs.at(m_lossFunction)->Mask());
        if (m_lossFunction->Output().GetDataType() == DataType::Float)
            rootGradientValue->Data()->SetValue(1.0f);
        else
            rootGradientValue->Data()->SetValue(1.0);

        auto modelParameters = m_combinedTrainingFunction->Parameters();
        std::unordered_map<Variable, ValuePtr> parameterGradients;
        for (const auto& parameter : modelParameters)
            parameterGradients[parameter] = nullptr;

        m_combinedTrainingFunction->Backward(backPropSate, { { m_lossFunction, rootGradientValue } }, parameterGradients);

        m_prevMinibatchNumSamples = GetSampleCountFromArguments(*(m_lossFunction->Arguments().begin()), arguments);

        bool anyUpdatesPerformed = false;
        for (auto learner : m_parameterLearners)
        {
            std::unordered_map<Parameter, NDArrayViewPtr> learnerParameterGradients;
            const auto& learnerParameters = learner->Parameters();
            for (const auto& parameter : learnerParameters)
            {
                learnerParameterGradients[parameter] = parameterGradients[parameter]->Data();

                if (parameterGradients[parameter]->Mask())
                    LogicError("The gradient value for a Parameter cannot have an associated mask!");
            }

            anyUpdatesPerformed |= learner->Update(learnerParameterGradients, m_prevMinibatchNumSamples);
        }

        return anyUpdatesPerformed;
    }

    static std::wstring GetTrainerStateCheckpointFilePath(const std::wstring& modelFilePath)
    {
        const wchar_t* checkpointExt = L".ckp";
        return modelFilePath + checkpointExt;
    }

    std::shared_ptr<std::fstream> GetFstream(const std::wstring& filePath, bool readOnly)
    {
        std::ios_base::openmode mode = std::ios_base::binary | (readOnly ? std::ios_base::in : std::ios_base::out);
#ifdef _MSC_VER
        return std::make_shared<std::fstream>(filePath, mode);
#else
        return std::make_shared<std::fstream>(wtocharpath(filePath.c_str()).c_str(), mode);
#endif
    }

    void Trainer::SaveCheckpoint(const std::wstring& modelFilePath)
    {
        SaveAsLegacyModel(m_combinedTrainingFunction, modelFilePath);

        if (m_parameterLearners.size() > 1)
            LogicError("Trainer::SaveCheckpoint: Checkpointing is currently unsupported for multiple learners");

        auto learnerState = (*(m_parameterLearners.begin()))->GetCheckpointState();
        std::wstring trainerStateCheckpointFilePath = GetTrainerStateCheckpointFilePath(modelFilePath);
        auto ckpStream = GetFstream(trainerStateCheckpointFilePath, false);
        *ckpStream << learnerState;
        ckpStream->flush();
    }

    void Trainer::RestoreFromCheckpoint(const std::wstring& modelFilePath)
    {
        auto firstLearner = *(m_parameterLearners.begin());

        auto loadedModelFunction = LoadLegacyModel(m_combinedTrainingFunction->Outputs()[0].GetDataType(), modelFilePath, DeviceDescriptor::CPUDevice());

        // TODO: Make sure that the loaded model is the same as the trainer's model through UID matching in the V2 format
        // TODO: For V1 format models make sure that the loaded model is isomorphic to the trainer's model
        auto loadedModelLeafVariables = loadedModelFunction->Inputs();
        auto trainerModelLeafVariables = m_combinedTrainingFunction->Inputs();
        if (trainerModelLeafVariables.size() != loadedModelLeafVariables.size())
            InvalidArgument("The loaded model's leaf variables do not match the trainer model's leaf variables");

        std::map<std::wstring, Variable> loadedModelLeafVariablesMap;
        for (auto leafVar : loadedModelLeafVariables)
            loadedModelLeafVariablesMap[leafVar.Uid()] = leafVar;

        std::map<std::wstring, Variable> trainerModelLeafVariablesMap;
        for (auto leafVar : trainerModelLeafVariables)
            trainerModelLeafVariablesMap[leafVar.Uid()] = leafVar;

        // Remove the initial state inputs of PastValue and FutureValue functions from the maps if they are a scalar constant
        // since these are not part of the internal CNTK serialized computation graph
        auto removePastAndFutureValueInitialStateScalarConstants = [](const std::unordered_set<FunctionPtr>& allPrimitiveFunctions, std::map<std::wstring, Variable>& modelLeafVariableMap) {
            for (auto funcPtr : allPrimitiveFunctions)
            {
                auto primitiveFunction = dynamic_cast<const PrimitiveFunction*>(funcPtr.get());
                if ((primitiveFunction->OpType() == PrimitiveOpType::PastValue) || (primitiveFunction->OpType() == PrimitiveOpType::FutureValue))
                {
                    auto initialStateInput = primitiveFunction->Inputs()[1];
                    if (initialStateInput.IsConstant() && (initialStateInput.Shape().TotalSize() == 1))
                        modelLeafVariableMap.erase(initialStateInput.Uid());
                }
            }
        };

        auto loadedModelCompositeFunction = dynamic_cast<const CompositeFunction*>(loadedModelFunction.get());
        removePastAndFutureValueInitialStateScalarConstants(loadedModelCompositeFunction->m_allPrimitiveFunctions, loadedModelLeafVariablesMap);

        auto trainerModelCompositeFunction = dynamic_cast<const CompositeFunction*>(m_combinedTrainingFunction.get());
        removePastAndFutureValueInitialStateScalarConstants(trainerModelCompositeFunction->m_allPrimitiveFunctions, trainerModelLeafVariablesMap);

        // Now update the trainer's model parameters and constants with those from the loaded model
        for (auto nameVarPair : trainerModelLeafVariablesMap)
        {
            auto trainerModelLeafVar = nameVarPair.second;

            auto areVariablesEquivalent = [](const Variable& left, const Variable& right) {
                return ((left.Kind() == right.Kind()) &&
                    ((left.Shape() == right.Shape()) || (AsTensorShape(left.Shape()) == AsTensorShape(right.Shape()))) &&
                    (left.GetDataType() == right.GetDataType()) &&
                    (left.DynamicAxes().size() == right.DynamicAxes().size()) &&
                    (left.NeedsGradient() == right.NeedsGradient()) &&
                    (left.Uid() == right.Uid()) &&
                    (left.IsSparse() == right.IsSparse()));
            };

            auto correspondingLoadedModelVar = loadedModelLeafVariablesMap.at(trainerModelLeafVar.Uid());

            if (!areVariablesEquivalent(correspondingLoadedModelVar, trainerModelLeafVar))
                InvalidArgument("The loaded model's leaf variables do not match the trainer model's leaf variables");

            if (trainerModelLeafVar.IsConstant() || trainerModelLeafVar.IsParameter())
            {
                auto trainerModelVarValue = trainerModelLeafVar.IsConstant() ? Constant(trainerModelLeafVar).Value() : Parameter(trainerModelLeafVar).Value();
                auto loadedModelVarValue = correspondingLoadedModelVar.IsConstant() ? Constant(correspondingLoadedModelVar).Value() : Parameter(correspondingLoadedModelVar).Value();
                trainerModelVarValue->CopyFrom(*loadedModelVarValue);
            }
        }

        if (m_parameterLearners.size() > 1)
            LogicError("Trainer::RestoreFromCheckpoint: Checkpointing is currently unsupported for multiple learners");

        std::wstring trainerStateCheckpointFilePath = GetTrainerStateCheckpointFilePath(modelFilePath);
        auto ckpStream = GetFstream(trainerStateCheckpointFilePath, true);
        Dictionary learnerState;
        *ckpStream >> learnerState;
        firstLearner->RestoreFromCheckpoint(learnerState);
    }

    double Trainer::PreviousMinibatchLossAverage() const
    {
        return (GetScalarValue(m_prevMinibatchAggregateTrainingLossValue) / m_prevMinibatchNumSamples);
    }

    double Trainer::PreviousMinibatchEvaluationAverage() const
    {
        if (!m_evaluationFunction)
            InvalidArgument("Trainer::PreviousMinibatchEvaluationAverage: Cannot get evaluation criterion value when no evaluation function was specified during 'this' trainer's construction");

        return (GetScalarValue(m_prevMinibatchAggregateEvalCriterionValue) / m_prevMinibatchNumSamples);
    }
}