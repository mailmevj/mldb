
#include "em.h"
#include "mldb/ml/em.h"
#include "matrix.h"
#include "mldb/server/mldb_server.h"
#include "mldb/server/procedure_collection.h"
#include "mldb/server/function_collection.h"
#include "jml/stats/distribution.h"
#include <boost/multi_array.hpp>
#include "jml/utils/guard.h"
#include "jml/utils/worker_task.h"
#include "jml/utils/pair_utils.h"
#include "mldb/arch/timers.h"

#include "jml/utils/vector_utils.h"
#include "mldb/types/basic_value_descriptions.h"

#include "mldb/sql/sql_expression.h"
#include "mldb/server/analytics.h"
#include "mldb/types/any_impl.h"

#include "jml/utils/smart_ptr_utils.h"


using namespace std;


namespace Datacratic {
namespace MLDB {

std::vector<double> tovector(Eigen::MatrixXd& m)
{
    std::vector<double> embedding;
    for(int i = 0; i < m.rows(); i++)
    {
        for(int j = 0; j < m.cols(); j++) {
             embedding.push_back(m(i,j)); // multiply by elements on diagonal
        }
    }       

    return embedding;    
}

DEFINE_STRUCTURE_DESCRIPTION(EMConfig);

EMConfigDescription::
EMConfigDescription()
{
    addField("dataset", &EMConfig::dataset,
             "Dataset provided for input to the k-means procedure.  This should be "
             "organized as an embedding, with each selected row containing the same "
             "set of columns with numeric values to be used as coordinates.");
    addField("output", &EMConfig::output,
             "Dataset for cluster assignment.  This dataset will contain the same "
             "row names as the input dataset, but the coordinates will be replaced "
             "by a single column giving the cluster number that the row was assigned "
             "to.");
    addField("centroids", &EMConfig::centroids,
             "Dataset in which the centroids will be recorded.  This dataset will "
             "have the same coordinates (columns) as those selected from the input "
             "dataset, but will have one row per cluster, providing the centroid of "
             "the cluster.");
    addField("select", &EMConfig::select,
             "Columns to select from the input matrix for the coordinates to input "
             "into k-means training.  The selected columns must be finite numbers "
             "and must not have missing values.",
             SelectExpression("*"));
    addField("where", &EMConfig::where,
             "Rows to select for k-means training.  This expression allows a subset "
             "of the rows that were input to the training process to be selected.",
             SqlExpression::parse("true"));
    addField("numInputDimensions", &EMConfig::numInputDimensions,
             "Number of dimensions from the input to use (-1 = all).  This limits "
             "the number of columns used.  Columns will be ordered alphabetically "
             "and the lowest ones kept.",
             -1);
    addField("numClusters", &EMConfig::numClusters,
             "Number of clusters to create.  This will provide the total number of "
             "centroids created.  There must be at least as many rows selected as "
             "clusters.", 10);
    addField("maxIterations", &EMConfig::maxIterations,
             "Maximum number of iterations to perform.  If no convergeance is "
             "reached within this number of iterations, the current clustering "
             "will be returned.", 2);
}

/*****************************************************************************/
/* EM PROCEDURE                                                           */
/*****************************************************************************/

EMProcedure::
EMProcedure(MldbServer * owner,
               PolyConfig config,
               const std::function<bool (const Json::Value &)> & onProgress)
    : Procedure(owner)
{
    this->emConfig = config.params.convert<EMConfig>();
}

Any
EMProcedure::
getStatus() const
{
    return Any();

}

RunOutput
EMProcedure::
run(const ProcedureRunConfig & run,
      const std::function<bool (const Json::Value &)> & onProgress) const
{
   ML::EstimationMaximisation em;

    auto onProgress2 = [&] (const Json::Value & progress)
        {
            Json::Value value;
            value["dataset"] = progress;
            return onProgress(value);
        };

    SqlExpressionMldbContext context(server);

    //cerr << "binding dataset" << endl;
    auto boundDataset = emConfig.dataset->bind(context);

    auto embeddingOutput = getEmbedding(emConfig.select, *boundDataset.dataset, boundDataset.asName,
                                        
                                        WhenExpression::parse("true"),
                                        emConfig.where, {},
                                        emConfig.numInputDimensions, 
                                        ORDER_BY_NOTHING,
                                        0, -1,
                                        onProgress2);

    auto rows = embeddingOutput.first;
    std::vector<KnownColumn> & vars = embeddingOutput.second;

    std::vector<ColumnName> columnNames;
    for (auto & v: vars) {
        columnNames.push_back(v.columnName);
    }

    std::vector<ML::distribution<float> > vecs;

    for (unsigned i = 0;  i < rows.size();  ++i) {
        vecs.emplace_back(ML::distribution<float>(std::get<2>(rows[i]).begin(),
                                                  std::get<2>(rows[i]).end()));
    }

    vector<int> inCluster;

    int numClusters = emConfig.numClusters;
    int numIterations = emConfig.maxIterations;

    //cerr << "EM training start" << endl;
    em.train(vecs, inCluster, numClusters, numIterations, 0);
    //cerr << "EM training end" << endl;

    // output
    
    if (emConfig.output.type != "" || emConfig.output.id != "") {
        auto output = createDataset(server, emConfig.output, onProgress2, true /*overwrite*/);

        Date applyDate = Date::now();
        
        for (unsigned i = 0;  i < rows.size();  ++i) {
            std::vector<std::tuple<ColumnName, CellValue, Date> > cols;
            cols.emplace_back(ColumnName("cluster"), inCluster[i], applyDate);
            output->recordRow(std::get<1>(rows[i]), cols);
        }
        
        output->commit();
    }

    //cerr << "centroids type" << emConfig.centroids.type << endl;
    if (emConfig.centroids.type != "" || emConfig.centroids.id != "") {

        //cerr << "writing centroids" << emConfig.centroids.type << endl;
        auto centroids = createDataset(server, emConfig.centroids, onProgress2, true /*overwrite*/);

        Date applyDate = Date::now();

        for (unsigned i = 0;  i < em.clusters.size();  ++i) {
            auto & cluster = em.clusters[i];

            std::vector<std::tuple<ColumnName, CellValue, Date> > cols;

            for (unsigned j = 0;  j < cluster.centroid.size();  ++j) {
                cols.emplace_back(columnNames[j], cluster.centroid[j], applyDate);
            }

            auto flatmatrix = tovector(cluster.covarianceMatrix);

            for (unsigned j = 0;  j < flatmatrix.size();  ++j) {
                cols.emplace_back(ColumnName(ML::format("c%02d", j)), flatmatrix[j], applyDate);
            }
            
            centroids->recordRow(RowName(ML::format("%i", i)), cols);
        }
        
        centroids->commit();
    }   

    return Any();
}

DEFINE_STRUCTURE_DESCRIPTION(EMFunctionConfig);

EMFunctionConfigDescription::
EMFunctionConfigDescription()
{
    addField("centroids", &EMFunctionConfig::centroids,
             "Dataset containing centroids of each cluster: one row per cluster.");
    addField("select", &EMFunctionConfig::select,
             "Fields to select to calculate k-means over.  Only those fields "
             "that are selected here need to be matched.  Default is to use "
             "all fields.",
             SelectExpression("*"));
    addField("where", &EMFunctionConfig::where,
             "Rows to select for k-means training.  This will effectively "
             "limit which clusters are active.  Default is to use all "
             "clusters.",
             SqlExpression::parse("true"));
}


/*****************************************************************************/
/* EM FUNCTION                                                              */
/*****************************************************************************/

EMFunction::
EMFunction(MldbServer * owner,
            PolyConfig config,
            const std::function<bool (const Json::Value &)> & onProgress)
    : Function(owner)
{
    functionConfig = config.params.convert<EMFunctionConfig>();

    auto dataset = obtainDataset(server, functionConfig.centroids, onProgress);

    //cerr << "loading embedding" << endl;

    // Load up the embeddings
    auto embeddingOutput = getEmbedding(functionConfig.select, *dataset, "",                                        
                                        WhenExpression::parse("true"),
                                        functionConfig.where, {},
                                        -1, 
                                        ORDER_BY_NOTHING,
                                        0, -1,
                                        onProgress);
    
    // Each row is a cluster
    auto rows = embeddingOutput.first;
    std::vector<KnownColumn> & vars = embeddingOutput.second;

    for (auto & v: vars) {
        columnNames.push_back(v.columnName);
    }

    int numCol = columnNames.size();
    numDim = ((sqrt(1+4*numCol)) - 1) / 2;
    
    for (auto & r: rows) {
        Cluster cluster;
        cluster.clusterName = jsonDecodeStr<CellValue>(std::get<1>(r).toString());
       
        std::vector<double>& values = std::get<2>(r);
        for (int i = 0; i < numDim; ++i)
        {
            cluster.centroid.push_back(values[i]);
        }
        cluster.covarianceMatrix = Eigen::MatrixXd(numDim, numDim);
        for (int i = 0; i < numDim; ++i)
        {
            for (int j = 0; j < numDim; ++j)
            {
              cluster.covarianceMatrix(i,j) = values[numDim + i*numDim + j];
            }
        }

        clusters.emplace_back(std::move(cluster));
    }

    //cerr << "got " << clusters.size()
    //     << " clusters with " << columnNames.size()
    //     << "values" << endl;
}

Any
EMFunction::
getStatus() const
{
    return Any();
}

FunctionOutput
EMFunction::
apply(const FunctionApplier & applier,
      const FunctionContext & context) const
{
    FunctionOutput result;

    ExpressionValue storage;
    const ExpressionValue & inputVal = context.get("embedding", storage);
    //cerr << "getting embedding" << endl;
    ML::distribution<float> input = inputVal.getEmbedding(numDim);
    Date ts = inputVal.getEffectiveTimestamp();

    double bestDist = INFINITY;
    CellValue bestCluster;

    for (unsigned i = 0;  i < clusters.size();  ++i) {
        double dist = 0.0f;
        if (dist < bestDist
            || (dist == bestDist && clusters[i].clusterName < bestCluster)) {
            bestDist = dist;
            bestCluster = clusters[i].clusterName;
        }
    }

    result.set("cluster", ExpressionValue(bestCluster, ts)); 
 
    return result;
}

FunctionInfo
EMFunction::
getFunctionInfo() const
{
    FunctionInfo result;

    result.input.addEmbeddingValue("embedding", columnNames.size());
    result.output.addAtomValue("cluster");

    return result;
}


namespace {

RegisterProcedureType<EMProcedure, EMConfig>
regEM(builtinPackage(), "EM.train",
          "Estimation-Maximisation; Generic clustering algorithm based on making statistical models converge",
          "procedures/KmeansProcedure.md.html");

RegisterFunctionType<EMFunction, EMFunctionConfig>
regEMFunction(builtinPackage(), "EM",
               "Apply an Estimation-Maximization clustering to new data",
               "functions/Kmeans.md.html");

} // file scope

} // namespace MLDB
} // namespace Datacratic