#include "openbr_internal.h"
#include "openbr/core/opencvutils.h"

using namespace std;
using namespace cv;

namespace br
{

/*!
 * \ingroup transforms
 * \brief Wraps OpenCV's random trees framework
 * \author Scott Klum \cite sklum
 * \brief http://docs.opencv.org/modules/ml/doc/random_trees.html
 */
class ForestTransform : public Transform
{
    Q_OBJECT

    void train(const TemplateList &data)
    {
        trainForest(data);
    }

    void project(const Template &src, Template &dst) const
    {
        dst = src;

        float response;
        if (classification && returnConfidence) {
            // Fuzzy class label
            response = forest.predict_prob(src.m().reshape(1,1));
        } else {
            response = forest.predict(src.m().reshape(1,1));
        }

        if (overwriteMat) {
            dst.m() = Mat(1, 1, CV_32F);
            dst.m().at<float>(0, 0) = response;
        } else {
            dst.file.set(outputVariable, response);
        }
    }

    void load(QDataStream &stream)
    {
        OpenCVUtils::loadModel(forest,stream);
    }

    void store(QDataStream &stream) const
    {
        OpenCVUtils::storeModel(forest,stream);
    }

    void init()
    {
        if (outputVariable.isEmpty())
            outputVariable = inputVariable;
    }

protected:
    Q_ENUMS(TerminationCriteria)
    Q_PROPERTY(bool classification READ get_classification WRITE set_classification RESET reset_classification STORED false)
    Q_PROPERTY(float splitPercentage READ get_splitPercentage WRITE set_splitPercentage RESET reset_splitPercentage STORED false)
    Q_PROPERTY(int maxDepth READ get_maxDepth WRITE set_maxDepth RESET reset_maxDepth STORED false)
    Q_PROPERTY(int maxTrees READ get_maxTrees WRITE set_maxTrees RESET reset_maxTrees STORED false)
    Q_PROPERTY(float forestAccuracy READ get_forestAccuracy WRITE set_forestAccuracy RESET reset_forestAccuracy STORED false)
    Q_PROPERTY(bool returnConfidence READ get_returnConfidence WRITE set_returnConfidence RESET reset_returnConfidence STORED false)
    Q_PROPERTY(bool overwriteMat READ get_overwriteMat WRITE set_overwriteMat RESET reset_overwriteMat STORED false)
    Q_PROPERTY(QString inputVariable READ get_inputVariable WRITE set_inputVariable RESET reset_inputVariable STORED false)
    Q_PROPERTY(QString outputVariable READ get_outputVariable WRITE set_outputVariable RESET reset_outputVariable STORED false)
    Q_PROPERTY(bool weight READ get_weight WRITE set_weight RESET reset_weight STORED false)
    Q_PROPERTY(TerminationCriteria termCrit READ get_termCrit WRITE set_termCrit RESET reset_termCrit STORED false)

public:
    enum TerminationCriteria { Iter = CV_TERMCRIT_ITER,
                EPS = CV_TERMCRIT_EPS,
                Both = CV_TERMCRIT_EPS | CV_TERMCRIT_ITER};

protected:
    BR_PROPERTY(bool, classification, true)
    BR_PROPERTY(float, splitPercentage, .01)
    BR_PROPERTY(int, maxDepth, std::numeric_limits<int>::max())
    BR_PROPERTY(int, maxTrees, 10)
    BR_PROPERTY(float, forestAccuracy, .1)
    BR_PROPERTY(bool, returnConfidence, true)
    BR_PROPERTY(bool, overwriteMat, true)
    BR_PROPERTY(QString, inputVariable, "Label")
    BR_PROPERTY(QString, outputVariable, "")
    BR_PROPERTY(bool, weight, false)
    BR_PROPERTY(TerminationCriteria, termCrit, Iter)

    CvRTrees forest;

    void trainForest(const TemplateList &data)
    {
        Mat samples = OpenCVUtils::toMat(data.data());
        Mat labels = OpenCVUtils::toMat(File::get<float>(data, inputVariable));

        Mat types = Mat(samples.cols + 1, 1, CV_8U);
        types.setTo(Scalar(CV_VAR_NUMERICAL));

        if (classification) {
            types.at<char>(samples.cols, 0) = CV_VAR_CATEGORICAL;
        } else {
            types.at<char>(samples.cols, 0) = CV_VAR_NUMERICAL;
        }

        bool usePrior = classification && weight;
        float priors[2];
        if (usePrior) {
            int nonZero = countNonZero(labels);
            priors[0] = 1;
            priors[1] = (float)(samples.rows-nonZero)/nonZero;
        }

        int minSamplesForSplit = data.size()*splitPercentage;
        forest.train( samples, CV_ROW_SAMPLE, labels, Mat(), Mat(), types, Mat(),
                    CvRTParams(maxDepth,
                               minSamplesForSplit,
                               0,
                               false,
                               2,
                               usePrior ? priors : 0,
                               false,
                               0,
                               maxTrees,
                               forestAccuracy,
                               termCrit));

        if (Globals->verbose) {
            qDebug() << "Number of trees:" << forest.get_tree_count();

            if (classification) {
                QTime timer;
                timer.start();
                int correctClassification = 0;
                float regressionError = 0;
                for (int i=0; i<samples.rows; i++) {
                    float prediction = forest.predict_prob(samples.row(i));
                    int label = forest.predict(samples.row(i));
                    if (label == labels.at<float>(i,0)) {
                        correctClassification++;
                    }
                    regressionError += fabs(prediction-labels.at<float>(i,0));
                }

                qDebug("Time to classify %d samples: %d ms\n \
                       Classification Accuracy: %f\n \
                       MAE: %f\n \
                       Sample dimensionality: %d",
                       samples.rows,timer.elapsed(),(float)correctClassification/samples.rows,regressionError/samples.rows,samples.cols);
            }
        }
    }
};

BR_REGISTER(Transform, ForestTransform)

/*!
 * \ingroup transforms
 * \brief Wraps OpenCV's random trees framework to induce features
 * \author Scott Klum \cite sklum
 * \brief https://lirias.kuleuven.be/bitstream/123456789/316661/1/icdm11-camready.pdf
 */
class ForestInductionTransform : public ForestTransform
{
    Q_OBJECT
    Q_PROPERTY(bool useRegressionValue READ get_useRegressionValue WRITE set_useRegressionValue RESET reset_useRegressionValue STORED false)
    BR_PROPERTY(bool, useRegressionValue, false)

    int totalSize;
    QList< QList<const CvDTreeNode*> > nodes;

    void fillNodes()
    {
        for (int i=0; i<forest.get_tree_count(); i++) {
            nodes.append(QList<const CvDTreeNode*>());
            const CvDTreeNode* node = forest.get_tree(i)->get_root();

            // traverse the tree and save all the nodes in depth-first order
            for(;;)
            {
                CvDTreeNode* parent;
                for(;;)
                {
                    if( !node->left )
                        break;
                    node = node->left;
                }

                nodes.last().append(node);

                for( parent = node->parent; parent && parent->right == node;
                    node = parent, parent = parent->parent )
                    ;

                if( !parent )
                    break;

                node = parent->right;
            }

            totalSize += nodes.last().size();
        }
    }

    void train(const TemplateList &data)
    {
        trainForest(data);
        if (!useRegressionValue) fillNodes();
    }

    void project(const Template &src, Template &dst) const
    {
        dst = src;

        Mat responses;

        if (useRegressionValue) {
            responses = Mat::zeros(forest.get_tree_count(),1,CV_32F);
            for (int i=0; i<forest.get_tree_count(); i++) {
                responses.at<float>(i,0) = forest.get_tree(i)->predict(src.m().reshape(1,1))->value;
            }
        } else {
            responses = Mat::zeros(totalSize,1,CV_32F);
            int offset = 0;
            for (int i=0; i<nodes.size(); i++) {
                int index = nodes[i].indexOf(forest.get_tree(i)->predict(src.m().reshape(1,1)));
                responses.at<float>(offset+index,0) = 1;
                offset += nodes[i].size();
            }
        }

        dst.m() = responses;
    }

    void load(QDataStream &stream)
    {
        OpenCVUtils::loadModel(forest,stream);
        if (!useRegressionValue) fillNodes();

    }

    void store(QDataStream &stream) const
    {
        OpenCVUtils::storeModel(forest,stream);
    }
};

BR_REGISTER(Transform, ForestInductionTransform)

/*!
 * \ingroup transforms
 * \brief Wraps OpenCV's Ada Boost framework
 * \author Scott Klum \cite sklum
 * \brief http://docs.opencv.org/modules/ml/doc/boosting.html
 */
class AdaBoostTransform : public Transform
{
    Q_OBJECT
    Q_ENUMS(Type)
    Q_ENUMS(SplitCriteria)

    Q_PROPERTY(Type type READ get_type WRITE set_type RESET reset_type STORED false)
    Q_PROPERTY(SplitCriteria splitCriteria READ get_splitCriteria WRITE set_splitCriteria RESET reset_splitCriteria STORED false)
    Q_PROPERTY(int weakCount READ get_weakCount WRITE set_weakCount RESET reset_weakCount STORED false)
    Q_PROPERTY(float trimRate READ get_trimRate WRITE set_trimRate RESET reset_trimRate STORED false)
    Q_PROPERTY(int folds READ get_folds WRITE set_folds RESET reset_folds STORED false)
    Q_PROPERTY(int maxDepth READ get_maxDepth WRITE set_maxDepth RESET reset_maxDepth STORED false)
    Q_PROPERTY(bool returnConfidence READ get_returnConfidence WRITE set_returnConfidence RESET reset_returnConfidence STORED false)
    Q_PROPERTY(bool overwriteMat READ get_overwriteMat WRITE set_overwriteMat RESET reset_overwriteMat STORED false)
    Q_PROPERTY(QString inputVariable READ get_inputVariable WRITE set_inputVariable RESET reset_inputVariable STORED false)
    Q_PROPERTY(QString outputVariable READ get_outputVariable WRITE set_outputVariable RESET reset_outputVariable STORED false)

public:
    enum Type { Discrete = CvBoost::DISCRETE,
                Real = CvBoost::REAL,
                Logit = CvBoost::LOGIT,
                Gentle = CvBoost::GENTLE};

    enum SplitCriteria { Default = CvBoost::DEFAULT,
                         Gini = CvBoost::GINI,
                         Misclass = CvBoost::MISCLASS,
                         Sqerr = CvBoost::SQERR};

private:
    BR_PROPERTY(Type, type, Real)
    BR_PROPERTY(SplitCriteria, splitCriteria, Default)
    BR_PROPERTY(int, weakCount, 100)
    BR_PROPERTY(float, trimRate, .95)
    BR_PROPERTY(int, folds, 0)
    BR_PROPERTY(int, maxDepth, 1)
    BR_PROPERTY(bool, returnConfidence, true)
    BR_PROPERTY(bool, overwriteMat, true)
    BR_PROPERTY(QString, inputVariable, "Label")
    BR_PROPERTY(QString, outputVariable, "")

    CvBoost boost;

    void train(const TemplateList &data)
    {
        Mat samples = OpenCVUtils::toMat(data.data());
        Mat labels = OpenCVUtils::toMat(File::get<float>(data, inputVariable));

        Mat types = Mat(samples.cols + 1, 1, CV_8U);
        types.setTo(Scalar(CV_VAR_NUMERICAL));
        types.at<char>(samples.cols, 0) = CV_VAR_CATEGORICAL;

        CvBoostParams params;
        params.boost_type = type;
        params.split_criteria = splitCriteria;
        params.weak_count = weakCount;
        params.weight_trim_rate = trimRate;
        params.cv_folds = folds;
        params.max_depth = maxDepth;

        boost.train( samples, CV_ROW_SAMPLE, labels, Mat(), Mat(), types, Mat(),
                    params);
    }

    void project(const Template &src, Template &dst) const
    {
        dst = src;
        float response;
        if (returnConfidence) {
            response = boost.predict(src.m().reshape(1,1),Mat(),Range::all(),false,true)/weakCount;
        } else {
            response = boost.predict(src.m().reshape(1,1));
        }

        if (overwriteMat) {
            dst.m() = Mat(1, 1, CV_32F);
            dst.m().at<float>(0, 0) = response;
        } else {
            dst.file.set(outputVariable, response);
        }
    }

    void load(QDataStream &stream)
    {
        OpenCVUtils::loadModel(boost,stream);
    }

    void store(QDataStream &stream) const
    {
        OpenCVUtils::storeModel(boost,stream);
    }


    void init()
    {
        if (outputVariable.isEmpty())
            outputVariable = inputVariable;
    }
};

BR_REGISTER(Transform, AdaBoostTransform)

} // namespace br

#include "tree.moc"
