#pragma once
#ifndef OPENGM_SUBGRADIENT_SSVM_LEARNER_HXX
#define OPENGM_SUBGRADIENT_SSVM_LEARNER_HXX

#include <vector>
#include <opengm/inference/inference.hxx>
#include <opengm/graphicalmodel/weights.hxx>
#include <opengm/utilities/random.hxx>
#include <opengm/learning/gradient-accumulator.hxx>
#include <omp.h>


namespace opengm {
    namespace learning {



           
    template<class DATASET>
    class SubgradientSSVM
    {
    public: 
        typedef DATASET DatasetType;
        typedef typename DATASET::GMType   GMType; 
        typedef typename DATASET::GMWITHLOSS GMWITHLOSS;
        typedef typename DATASET::LossType LossType;
        typedef typename GMType::ValueType ValueType;
        typedef typename GMType::IndexType IndexType;
        typedef typename GMType::LabelType LabelType; 
        typedef opengm::learning::Weights<double> WeightsType;
        typedef typename std::vector<LabelType>::const_iterator LabelIterator;
        typedef FeatureAccumulator<GMType, LabelIterator> FeatureAcc;

        class Parameter{
        public:

            enum LearningMode{
                Online = 0,
                Batch = 2
            };


            Parameter(){
                eps_ = 0.00001;
                maxIterations_ = 10000;
                stopLoss_ = 0.0;
                learningRate_ = 1.0;
                C_ = 1.0;
                learningMode_ = Online;
            }       

            double eps_;
            size_t maxIterations_;
            double stopLoss_;
            double learningRate_;
            double C_;
            LearningMode learningMode_;
        };


        SubgradientSSVM(DATASET&, const Parameter& );

        template<class INF>
        void learn(const typename INF::Parameter& para); 
        //template<class INF, class VISITOR>
        //void learn(typename INF::Parameter para, VITITOR vis);

        const opengm::learning::Weights<double>& getWeights(){return weights_;}
        Parameter& getLerningParameters(){return para_;}


        double getLearningRate( )const{
            if(para_.decayExponent_<=0.000000001 && para_.decayExponent_>=-0.000000001 ){
                return 1.0;
            }
            else{
                return std::pow(para_.decayT0_ + static_cast<double>(iteration_),para_.decayExponent_);
            }
        }

    private:

        double updateWeights();

        DATASET& dataset_;
        WeightsType  weights_;
        Parameter para_;
        size_t iteration_;
        FeatureAcc featureAcc_;
    }; 

    template<class DATASET>
    SubgradientSSVM<DATASET>::SubgradientSSVM(DATASET& ds, const Parameter& p )
    : dataset_(ds), para_(p),iteration_(0),featureAcc_(ds.getNumberOfWeights())
    {
        featureAcc_.resetWeights();
        weights_ = opengm::learning::Weights<double>(ds.getNumberOfWeights());
  
    }


    template<class DATASET>
    template<class INF>
    void SubgradientSSVM<DATASET>::learn(const typename INF::Parameter& para){


        typedef typename INF:: template RebindGm<GMWITHLOSS>::type InfLossGm;
        typedef typename InfLossGm::Parameter InfLossGmParam;
        InfLossGmParam infLossGmParam(para);


        const size_t nModels = dataset_.getNumberOfModels();
        const size_t nWegihts = dataset_.getNumberOfWeights();

        




        if(para_.learningMode_ == Parameter::Online){
            RandomUniform<size_t> randModel(0, nModels);
            std::cout<<"online mode\n";
            for(iteration_=0 ; iteration_<para_.maxIterations_; ++iteration_){

                if(iteration_%nModels==0){
                    std::cout<<"loss :"<<dataset_. template getTotalLoss<INF>(para)<<"\n";
                }


                // get random model
                const size_t gmi = randModel();
                // lock the model
                dataset_.lockModel(gmi);
                const GMWITHLOSS & gmWithLoss = dataset_.getModelWithLoss(gmi);

                // do inference
                std::vector<LabelType> arg;
                opengm::infer<InfLossGm>(gmWithLoss, infLossGmParam, arg);
                featureAcc_.resetWeights();
                featureAcc_.accumulateModelFeatures(dataset_.getModel(gmi), dataset_.getGT(gmi).begin(), arg.begin());
                dataset_.unlockModel(gmi);

                // update weights
                const double wChange =updateWeights();

            }
        }
        else if(para_.learningMode_ == Parameter::Batch){
            std::cout<<"batch mode\n";
            for(iteration_=0 ; iteration_<para_.maxIterations_; ++iteration_){
                // this 
                if(iteration_%1==0){
                    std::cout<<"loss :"<<dataset_. template getTotalLoss<INF>(para)<<"\n";
                }

                // reset the weights
                featureAcc_.resetWeights();



                omp_lock_t modelLockUnlock;
                omp_init_lock(&modelLockUnlock);

                omp_lock_t featureAccLock;
                omp_init_lock(&featureAccLock);


                #pragma omp parallel for
                for(size_t gmi=0; gmi<nModels; ++gmi)
                {
                    
                    // lock the model
                    omp_set_lock(&modelLockUnlock);
                    dataset_.lockModel(gmi);     
                    omp_unset_lock(&modelLockUnlock);
                        
                    

                    const GMWITHLOSS & gmWithLoss = dataset_.getModelWithLoss(gmi);
                    //run inference
                    std::vector<LabelType> arg;
                    opengm::infer<InfLossGm>(gmWithLoss, infLossGmParam, arg);


                    // 
                    FeatureAcc featureAcc(nWegihts);
                    featureAcc.accumulateModelFeatures(dataset_.getModel(gmi), dataset_.getGT(gmi).begin(), arg.begin());


                    // acc features
                    omp_set_lock(&featureAccLock);
                    featureAcc_.accumulateFromOther(featureAcc);
                    omp_unset_lock(&featureAccLock);

                    // unlock the model
                    omp_set_lock(&modelLockUnlock);
                    dataset_.unlockModel(gmi);     
                    omp_unset_lock(&modelLockUnlock);
                }

                // update the weights
                const double wChange =updateWeights();

            }
        }

        weights_ = dataset_.getWeights();
    }


    template<class DATASET>
    double SubgradientSSVM<DATASET>::updateWeights(){

        const size_t nWegihts = dataset_.getNumberOfWeights();

        WeightsType p(nWegihts);

        if(para_.learningMode_ == Parameter::Batch){
            for(size_t wi=0; wi<nWegihts; ++wi){
                p[wi] =  dataset_.getWeights().getWeight(wi);
                p[wi] += para_.C_ * featureAcc_.getWeight(wi)/double(dataset_.getNumberOfModels());
            }
        }
        else{
            for(size_t wi=0; wi<nWegihts; ++wi){
                p[wi] =  dataset_.getWeights().getWeight(wi);
                p[wi] += para_.C_ * featureAcc_.getWeight(wi);
            }
        }


        double wChange = 0.0;
        
        for(size_t wi=0; wi<nWegihts; ++wi){
            const double wOld = dataset_.getWeights().getWeight(wi);
            const double wNew = wOld - (para_.learningRate_/double(iteration_+1))*p[wi];
            wChange += std::pow(wOld-wNew,2);
            dataset_.getWeights().setWeight(wi, wNew);
        }
        weights_ = dataset_.getWeights();
        return wChange;
    }
}
}
#endif
