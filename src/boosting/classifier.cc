/* classifier.cc
   Jeremy Barnes, 6 June 2003
   Copyright (c) 2003 Jeremy Barnes.  All rights reserved.

   Implementation of basic classifier methods.
*/

#include "classifier.h"
#include "classifier_persist_impl.h"
#include "arch/threads.h"
#include "utils/file_functions.h"
#include "ace/OS.h"
#include "evaluation.h"
#include "config_impl.h"
#include "worker_task.h"
#include "utils/guard.h"
#include "dense_features.h"
#include "utils/smart_ptr_utils.h"
#include "utils/vector_utils.h"
#include <boost/bind.hpp>
#include <boost/thread/tss.hpp>


using namespace std;
using namespace DB;



namespace ML {


/*****************************************************************************/
/* OUTPUT_ENCODING                                                           */
/*****************************************************************************/

std::string print(Output_Encoding encoding)
{
    switch (encoding) {
    case OE_PROB:   return "PROB";
    case OE_PM_INF: return "PM_INF";
    case OE_PM_ONE: return "PM_ONE";
    default:        return format("Output_Encoding(%d)", encoding);
    }
}

BYTE_PERSISTENT_ENUM_IMPL(Output_Encoding);


/*****************************************************************************/
/* OPTIMIZATION_INFO                                                         */
/*****************************************************************************/

void
Optimization_Info::
apply(const Feature_Set & fset, float * output) const
{
    if (fset.size() != indexes.size())
        throw Exception("Optimization_Info::apply(): size mismatch");

    for (unsigned i = 0;  i < indexes.size();  ++i)
        if (indexes[i] != -1)
            output[indexes[i]] = fset[i].second;
}

void
Optimization_Info::
apply(const std::vector<float> & fset, float * output) const
{
    if (fset.size() != indexes.size())
        throw Exception("Optimization_Info::apply(): size mismatch");

    for (unsigned i = 0;  i < indexes.size();  ++i)
        if (indexes[i] != -1)
            output[indexes[i]] = fset[i];
}

void
Optimization_Info::
apply(const float * fset, float * output) const
{
    for (unsigned i = 0;  i < indexes.size();  ++i)
        if (indexes[i] != -1)
            output[indexes[i]] = fset[i];
}

int
Optimization_Info::
get_optimized_index(const Feature & feature) const
{
    map<Feature, int>::const_iterator it
        = feature_to_optimized_index.find(feature);
    if (it == feature_to_optimized_index.end())
        throw Exception("didn't find optimized feature index");
    return it->second;
}


/*****************************************************************************/
/* EXPLANATION                                                               */
/*****************************************************************************/

Explanation::
Explanation(const Feature_Set & fset,
            const Feature_Space & fspace,
            int label)
    : value(0.0), bias(0.0), fset(&fset), fspace(&fspace), label(label)
{
}

void
Explanation::
add(const Explanation & other, double weight)
{
    for (Feature_Weights::const_iterator
             it = other.feature_weights.begin(),
             end = other.feature_weights.end();
         it != end;  ++it)
        feature_weights[it->first] += weight * it->second;
}

struct Sort_On_Abs_Second {
    template<class X>
    bool operator () (const X & x1, const X & x2)
    {
        return abs(x1.second) > abs(x2.second);
    }
};

std::string
Explanation::
print(int nfeatures) const
{
    // Rank the features
    vector<pair<Feature, float> > ranked(feature_weights.begin(),
                                         feature_weights.end());

    std::sort(ranked.begin(), ranked.end(),
              Sort_On_Abs_Second());
    
    std::string result;
    if (bias != 0.0)
        result += format("%12.6f                      BIAS\n",
                         bias);
    for (unsigned i = 0;  i < ranked.size() && i < nfeatures;  ++i) {
        const Feature & feature = ranked[i].first;
        float score = ranked[i].second;
        result += format("%12.6f %-20s %s\n",
                         score,
                         fspace->print(feature, (*fset)[feature]).c_str(),
                         fspace->print(feature).c_str());
    }

    double total = bias;
    for (unsigned i = 0;  i < ranked.size();  ++i)
        total += ranked[i].second;
    

    result += format("%12.6f                      TOTAL\n",
                     total);

    return result;
}


/*****************************************************************************/
/* CLASSIFIER_IMPL                                                           */
/*****************************************************************************/

Classifier_Impl::Classifier_Impl()
    : label_count_(0)
{
}

namespace {

size_t get_label_count(const Feature_Space & fs,
                       const Feature & predicted)
{
    Feature_Info info = fs.info(predicted);
    return info.value_count();
}

size_t check_label_count(const Feature_Space & fs,
                         const Feature & predicted,
                         size_t label_count)
{
    /* Don't try to check if we didn't put a feature in. */
    if (predicted == MISSING_FEATURE) return label_count;

    Feature_Info info = fs.info(predicted);

    /* For reals, we assume the number of labels are known. */
    if (info.type() != REAL
        && info.value_count() != label_count) {
        

        throw Exception("Classifier_Impl: check_label_count(): feature (" 
                        + ostream_format(info.value_count())
                        + ") and label ("
                        + ostream_format(label_count)
                        + ") counts don't match; feature = "
                        + fs.print(predicted));
    }

    return label_count;
}

} // file scope

Classifier_Impl::
Classifier_Impl(const boost::shared_ptr<const Feature_Space> & feature_space,
                const Feature & predicted)
    : feature_space_(feature_space), predicted_(predicted),
      label_count_(get_label_count(*feature_space, predicted))
{
}

Classifier_Impl::
Classifier_Impl(const boost::shared_ptr<const Feature_Space> & feature_space,
                const Feature & predicted,
                size_t label_count)
    : feature_space_(feature_space), predicted_(predicted),
      label_count_(check_label_count(*feature_space, predicted, label_count))
{
}

void Classifier_Impl::
init(const boost::shared_ptr<const Feature_Space> & feature_space,
     const Feature & predicted)
{
    feature_space_ = feature_space;
    predicted_ = predicted;
    label_count_ = get_label_count(*feature_space, predicted);
}

void Classifier_Impl::
init(const boost::shared_ptr<const Feature_Space> & feature_space,
     const Feature & predicted,
     size_t label_count)
{
    feature_space_ = feature_space;
    predicted_ = predicted;
    label_count_ = check_label_count(*feature_space, predicted, label_count);
}

Classifier_Impl::~Classifier_Impl()
{
}

float Classifier_Impl::predict(int label, const Feature_Set & features) const
{
    if (label < 0 || label >= label_count())
        throw Exception(format("Attempt to predict non-existent label: "
                               "label = %d, label_count = %zd", label,
                               label_count()));
    return predict(features)[label];
}

int Classifier_Impl::predict_highest(const Feature_Set & features) const
{
    distribution<float> prediction = predict(features);
    return std::max_element(prediction.begin(), prediction.end())
        - prediction.begin();
}

Optimization_Info
Classifier_Impl::
optimize(const std::vector<Feature> & features)
{
    Optimization_Info result;
    result.from_features = features;
    result.to_features = all_features();

    if (!optimization_supported())
        return result;

    map<Feature, int> & feature_map = result.feature_to_optimized_index;
    for (unsigned i = 0;  i < result.to_features.size();  ++i) {
        feature_map[result.to_features[i]] = i;
    }

    int num_done = 0;
    result.indexes.resize(features.size());
    for (unsigned i = 0;  i < features.size();  ++i) {
        if (!feature_map.count(features[i])) {
            // We don't need this feature
            result.indexes[i] = -1;
        }
        else {
            result.indexes[i] = feature_map[features[i]];
            ++num_done;
        }
    }

    if (num_done != result.to_features.size())
        throw Exception("optimize(): didn't find all features needed for "
                        "classifier");

    result.initialized = true;

    optimize_impl(result);

    return result;
}

Optimization_Info
Classifier_Impl::
optimize(const Feature_Set & feature_set)
{
    if (!optimization_supported())
        return Optimization_Info();

    // Extract the list of features, and continue
    vector<Feature> features;
    features.reserve(feature_set.size());

    for (Feature_Set::const_iterator
             it = feature_set.begin(),
             end = feature_set.end();
         it != end;  ++it) {
        pair<Feature, float> val = *it;
        features.push_back(val.first);
    }

    return optimize(features);
}

bool
Classifier_Impl::
optimization_supported() const
{
    return false;
}

bool
Classifier_Impl::
predict_is_optimized() const
{
    return false;
}

Label_Dist
Classifier_Impl::
predict(const Feature_Set & features,
        const Optimization_Info & info) const
{
    if (!predict_is_optimized() || !info) return predict(features);

    float fv[info.features_out()];
    info.apply(features, fv);

    return optimized_predict_impl(fv, info);
}

Label_Dist
Classifier_Impl::
predict(const std::vector<float> & features,
        const Optimization_Info & info) const
{
    float fv[info.features_out()];
    info.apply(features, fv);

    return optimized_predict_impl(fv, info);
}

Label_Dist
Classifier_Impl::
predict(const float * features,
        const Optimization_Info & info) const
{
    float fv[info.features_out()];
    info.apply(features, fv);

    return optimized_predict_impl(fv, info);
}

float
Classifier_Impl::
predict(int label,
        const Feature_Set & features,
        const Optimization_Info & info) const
{
    if (!predict_is_optimized() || !info) return predict(label, features);

    float fv[info.features_out()];

    info.apply(features, fv);

    return optimized_predict_impl(label, fv, info);
}

float
Classifier_Impl::
predict(int label,
        const std::vector<float> & features,
        const Optimization_Info & info) const
{
    if (!predict_is_optimized() || !info) {

        // Convert to standard feature set, then call classical predict
        Dense_Feature_Set fset(make_unowned_sp(info.to_features),
                               &features[0]);

        return predict(label, fset);
    }

    float fv[info.features_out()];

    info.apply(features, fv);

    return optimized_predict_impl(label, fv, info);
}

float
Classifier_Impl::
predict(int label,
        const float * features,
        const Optimization_Info & info) const
{
    if (!predict_is_optimized() || !info) {

        // Convert to standard feature set, then call classical predict
        Dense_Feature_Set fset(make_unowned_sp(info.to_features),
                               features);

        return predict(label, fset);
    }

    float fv[info.features_out()];

    info.apply(features, fv);

    return optimized_predict_impl(label, fv, info);
}

bool
Classifier_Impl::
optimize_impl(Optimization_Info & info)
{
    return false;
}

Label_Dist
Classifier_Impl::
optimized_predict_impl(const float * features,
                       const Optimization_Info & info) const
{
    // If the classifier implemented optimized predict, then this would have
    // been overridden.
    
    // Convert to standard feature set, then call classical predict
    Dense_Feature_Set fset(make_unowned_sp(info.to_features),
                           features);
    
    return predict(fset);
}

void
Classifier_Impl::
optimized_predict_impl(const float * features,
                       const Optimization_Info & info,
                       double * accum,
                       double weight) const
{
    Label_Dist result = optimized_predict_impl(features, info);
    for (unsigned i = 0;  i < result.size();  ++i) {
        accum[i] += weight * result[i];
    }
}

float
Classifier_Impl::
optimized_predict_impl(int label,
                       const float * features,
                       const Optimization_Info & info) const
{
    // If the classifier implemented optimized predict, then this would have
    // been overridden.
    
    // Convert to standard feature set, then call classical predict
    Dense_Feature_Set fset(make_unowned_sp(info.to_features),
                           features);

    return predict(label, fset);
}

namespace {

struct Accuracy_Job_Info {
    const Training_Data & data;
    const distribution<float> & example_weights;
    const Classifier_Impl & classifier;
    const Optimization_Info & opt_info;
    Lock lock;
    double & correct;
    double & total;
    double & rmse_accum;

    Accuracy_Job_Info(const Training_Data & data,
                      const distribution<float> & example_weights,
                      const Classifier_Impl & classifier,
                      const Optimization_Info & opt_info,
                      double & correct, double & total,
                      double & rmse_accum)
        : data(data), example_weights(example_weights),
          classifier(classifier), opt_info(opt_info),
          correct(correct), total(total), rmse_accum(rmse_accum)
    {
    }

    void calc(int x_start, int x_end)
    {
        //cerr << "calculating from " << x_start << " to " << x_end << endl;

        double sub_total = 0.0, sub_correct = 0.0, sub_rmse = 0.0;

        bool regression_problem
            = classifier.feature_space()->info(classifier.predicted()).type()
            == REAL;

        for (unsigned x = x_start;  x < x_end;  ++x) {
            double w = (example_weights.empty() ? 1.0 : example_weights[x]);
            if (w == 0.0) continue;

            //cerr << "x = " << x << " w = " << w << endl;
            
            distribution<float> result = classifier.predict(data[x], opt_info);
            
            if (regression_problem) {
                float correct = data[x][classifier.predicted()];
                double error = 1.0 - correct;
                sub_rmse += w * error * error;
                sub_total += w;
            }
            else {
                Correctness c = correctness(result, classifier.predicted(),
                                            data[x]);
                sub_correct += w * c.possible * c.correct;
                sub_total += w * c.possible;
                sub_rmse += w * c.possible * c.margin * c.margin;
            }
        }

        Guard guard(lock);
        correct += sub_correct;
        total += sub_total;
        rmse_accum += sub_rmse;
    }
};

struct Accuracy_Job {
    Accuracy_Job(Accuracy_Job_Info & info,
                 int x_start, int x_end)
        : info(info), x_start(x_start), x_end(x_end)
    {
    }

    Accuracy_Job_Info & info;
    int x_start, x_end;
    
    void operator () () const
    {
        info.calc(x_start, x_end);
    }
};

} // file scope

std::pair<float, float>
Classifier_Impl::
accuracy(const Training_Data & data,
         const distribution<float> & example_weights,
         const Optimization_Info * opt_info_ptr) const
{
    double correct = 0.0;
    double total = 0.0;
    double rmse_accum = 0.0;

    if (!example_weights.empty()
        && example_weights.size() != data.example_count())
        throw Exception("Classifier_Impl::accuracy(): dataset and weight "
                        "vector sizes don't match");

    unsigned nx = data.example_count();

    Optimization_Info new_opt_info;
    const Optimization_Info & opt_info
        = (opt_info_ptr ? *opt_info_ptr : new_opt_info);

    Accuracy_Job_Info info(data, example_weights, *this, opt_info,
                           correct, total, rmse_accum);
    static Worker_Task & worker = Worker_Task::instance(num_threads() - 1);
    
    int group;
    {
        int parent = -1;  // no parent group
        group = worker.get_group(NO_JOB,
                                 format("accuracy group under %d", parent),
                                 parent);
        Call_Guard guard(boost::bind(&Worker_Task::unlock_group,
                                     boost::ref(worker),
                                     group));
        
        /* Do 1024 examples per job. */
        for (unsigned x = 0;  x < data.example_count();  x += 1024)
            worker.add(Accuracy_Job(info, x, std::min(x + 1024, nx)),
                       format("accuracy example %d to %d under %d",
                              x, x + 1024, group),
                       group);
    }

    worker.run_until_finished(group);
    
    return make_pair(correct / total, sqrt(rmse_accum / total));
}

namespace {

struct Predict_Job {

    int x_start, x_end;
    const Classifier_Impl & classifier;
    const Optimization_Info * opt_info;
    const Training_Data & data;

    Predict_Job(int x_start, int x_end,
                const Classifier_Impl & classifier,
                const Optimization_Info * opt_info,
                const Training_Data & data)
        : x_start(x_start), x_end(x_end),
          classifier(classifier), opt_info(opt_info),
          data(data)
    {
        
    }

    typedef void result_type;

    void operator () (Classifier_Impl::Predict_All_Output_Func output)
    {
        for (unsigned x = x_start;  x < x_end;  ++x) {
            Label_Dist prediction
                = opt_info
                ? classifier.predict(data[x], *opt_info)
                : classifier.predict(data[x]);
            output(x, &prediction[0]);
        }
    }

    void operator () (int label,
                      Classifier_Impl::Predict_One_Output_Func output)
    {
        for (unsigned x = x_start;  x < x_end;  ++x) {
            float prediction
                = opt_info
                ? classifier.predict(label, data[x], *opt_info)
                : classifier.predict(label, data[x]);
            output(x, prediction);
        }
    }
};

} // file scope

void
Classifier_Impl::
predict(const Training_Data & data,
        Predict_All_Output_Func output,
        const Optimization_Info * opt_info) const
{
    unsigned nx = data.example_count();

    static Worker_Task & worker = Worker_Task::instance(num_threads() - 1);
    
    int group;
    {
        int parent = -1;  // no parent group
        group = worker.get_group(NO_JOB,
                                 format("predict group under %d", parent),
                                 parent);
        Call_Guard guard(boost::bind(&Worker_Task::unlock_group,
                                     boost::ref(worker),
                                     group));
        
        /* Do 1024 examples per job. */
        for (unsigned x = 0;  x < data.example_count();  x += 1024)
            worker.add(boost::bind(Predict_Job(x,
                                               std::min(x + 1024, nx),
                                               *this,
                                               opt_info,
                                               data),
                                   output),
                       "predict job",
                       group);
    }

    worker.run_until_finished(group);
}

void
Classifier_Impl::
predict(const Training_Data & data,
        int label,
        Predict_One_Output_Func output,
        const Optimization_Info * opt_info) const
{
    unsigned nx = data.example_count();

    static Worker_Task & worker = Worker_Task::instance(num_threads() - 1);
    
    int group;
    {
        int parent = -1;  // no parent group
        group = worker.get_group(NO_JOB,
                                 format("predict group under %d", parent),
                                 parent);
        Call_Guard guard(boost::bind(&Worker_Task::unlock_group,
                                     boost::ref(worker),
                                     group));
        
        /* Do 1024 examples per job. */
        for (unsigned x = 0;  x < data.example_count();  x += 1024)
            worker.add(boost::bind(Predict_Job(x,
                                               std::min(x + 1024, nx),
                                               *this,
                                               opt_info,
                                               data),
                                   label,
                                   output),
                       "predict job",
                       group);
    }

    worker.run_until_finished(group);
}

Explanation
Classifier_Impl::
explain(const Feature_Set & feature_set,
        int label,
        double weight) const
{
    throw Exception("class " + demangle(typeid(*this).name())
                    + " doesn't implement the explain() method");
}


boost::shared_ptr<Classifier_Impl>
Classifier_Impl::
poly_reconstitute(DB::Store_Reader & store,
                  const boost::shared_ptr<const Feature_Space> & fs)
{
    compact_size_t fs_flag(store);
    //cerr << "fs_flag = " << fs_flag << endl;
    if (fs_flag) {
        boost::shared_ptr<Feature_Space> fs2;
        store >> fs2;  // ignore this one
        return Registry<Classifier_Impl>::singleton().reconstitute(store, fs);
    }
    else 
        return Registry<Classifier_Impl>::singleton().reconstitute(store, fs);
}

boost::shared_ptr<Classifier_Impl>
Classifier_Impl::poly_reconstitute(DB::Store_Reader & store)
{
    //cerr << __PRETTY_FUNCTION__ << endl;
    compact_size_t fs_flag(store);

    //cerr << "poly_reconstitute: fs_flag = " << fs_flag << endl;

    boost::shared_ptr<const Feature_Space> fs;
    boost::shared_ptr<Feature_Space> fs_mutable;
    if (fs_flag) {
        store >> fs_mutable;

        //cerr << "reconstituted feature space " << fs_mutable->print() << endl;

        fs = fs_mutable;
    }
    else fs = FS_Context::inner();

    //cerr << "reconstituting with feature space" << endl;

    boost::shared_ptr<Classifier_Impl> result
        = Registry<Classifier_Impl>::singleton().reconstitute(store, fs);

    //cerr << "result->predicted() = " << result->predicted() << endl;

    //cerr << "poly_reconstitute: feature_space = " << result->feature_space()
    //     << endl;

    if (fs_mutable) {
        //cerr << "freezing mutable" << endl;
        fs_mutable->freeze();
    }

    return result;
}


void Classifier_Impl::
poly_serialize(DB::Store_Writer & store, bool write_fs) const
{
    if (write_fs) {
        store << compact_size_t(1);
        store << feature_space();
    }
    else store << compact_size_t(0);

    Registry<Classifier_Impl>::singleton().serialize(store, this);
}

bool Classifier_Impl::merge_into(const Classifier_Impl & other, float)
{
    return false;
}

Classifier_Impl *
Classifier_Impl::merge(const Classifier_Impl & other, float) const
{
    return 0;
}

std::string
Classifier_Impl::
summary() const
{
    return class_id();
}


/*****************************************************************************/
/* POLYMORPHIC STUFF                                                         */
/*****************************************************************************/

namespace {

/* Put one of these objects per thread. */
boost::thread_specific_ptr<vector<boost::shared_ptr<const Feature_Space> > >
    fs_stack;

} // file scope

FS_Context::
FS_Context(const boost::shared_ptr<const Feature_Space> & feature_space)
{
    if (!fs_stack.get())
        fs_stack.reset(new vector<boost::shared_ptr<const Feature_Space> >());
    fs_stack->push_back(feature_space);
}

FS_Context::~FS_Context()
{
    if (!fs_stack.get())
        throw Exception("FS_Context never initialized");
    if (fs_stack->empty())
        throw Exception("FS stack was empty in destructor; bad problem");
    fs_stack->pop_back();
}

const boost::shared_ptr<const Feature_Space> & FS_Context::inner()
{
    if (!fs_stack.get())
        throw Exception("FS_Context never initialized");
    if (fs_stack->empty()) throw Exception("feature space stack is empty");
    return fs_stack->back();
}

DB::Store_Writer &
operator << (DB::Store_Writer & store,
             const boost::shared_ptr<const Classifier_Impl> & classifier)
{
    classifier->poly_serialize(store);
    return store;
}

DB::Store_Reader &
operator >> (DB::Store_Reader & store,
             boost::shared_ptr<Classifier_Impl> & classifier)
{
    Classifier_Impl::poly_reconstitute(store, FS_Context::inner());
    return store;
}


/*****************************************************************************/
/* CLASSIFIER                                                                */
/*****************************************************************************/

Classifier::Classifier()
{
}

Classifier::Classifier(const boost::shared_ptr<Classifier_Impl> & impl)
    : impl(impl)
{
}

Classifier::Classifier(Classifier_Impl * impl, bool take_copy)
{
    if (take_copy) this->impl.reset(impl->make_copy());
    else this->impl.reset(impl);
}

Classifier::Classifier(const Classifier_Impl & impl)
    : impl(impl.make_copy())
{
}

Classifier::
Classifier(const std::string & name,
           const boost::shared_ptr<const Feature_Space> & feature_space)
{
    throw Exception("Classifier::Classifier(string, fs): not implemented");
    //impl = Registry<Classifier_Impl>::singleton().create(name, feature_space);
}

Classifier::
Classifier(const boost::shared_ptr<const Feature_Space> & feature_space,
           DB::Store_Reader & store)
{
    impl = Classifier_Impl::poly_reconstitute(store, feature_space);
}

Classifier::Classifier(const Classifier & other)
    : impl(other.impl ? other.impl->make_copy() : 0)
{
}

Classifier::Classifier &
Classifier::operator = (const Classifier & other)
{
    if (other.impl) impl.reset(other.impl->make_copy());
    else impl.reset();
    return *this;
}

DB::Store_Writer &
operator << (DB::Store_Writer & store, const Classifier & classifier)
{
    classifier.serialize(store);
    return store;
}

DB::Store_Reader &
operator >> (DB::Store_Reader & store, Classifier & classifier)
{
    classifier.reconstitute(store, FS_Context::inner());
    return store;
}

void Classifier::load(const std::string & filename)
{
    Store_Reader store(filename);
    reconstitute(store);
}

void Classifier::
load(const std::string & filename, boost::shared_ptr<const Feature_Space> fs)
{
    Store_Reader store(filename);
    reconstitute(store, fs);
}

void Classifier::save(const std::string & filename, bool write_fs) const
{
    Store_Writer store(filename);
    serialize(store, write_fs);
}


/*****************************************************************************/
/* FACTORY_BASE<CLASSIFIER_IMPL>                                             */
/*****************************************************************************/

Factory_Base<Classifier_Impl>::~Factory_Base()
{
}

} // namespace ML


