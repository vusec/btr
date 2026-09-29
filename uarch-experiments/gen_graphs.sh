#!/bin/bash
set -e

DATA_FOLDER=results
GRAPH_FOLDER=graphs

python3 aggregate.py ${DATA_FOLDER}/log_U9_285K.txt ${DATA_FOLDER}/agg_U9_285K.txt > ${DATA_FOLDER}/summary_U9_285K.txt
python3 aggregate.py ${DATA_FOLDER}/log_Cortex_A76.txt ${DATA_FOLDER}/agg_A76.txt > ${DATA_FOLDER}/summary_U9_A76.txt
python3 aggregate.py ${DATA_FOLDER}/log_AMD_7950X.txt ${DATA_FOLDER}/agg_AMD_7950X.txt > ${DATA_FOLDER}/summary_AMD_7950X.txt
python3 aggregate.py ${DATA_FOLDER}/log_Cortex_X3.txt ${DATA_FOLDER}/agg_Cortex_X3.txt > ${DATA_FOLDER}/summary_Cortex_X3.txt
python3 aggregate.py ${DATA_FOLDER}/log_i9_14900K.txt ${DATA_FOLDER}/agg_i9_14900K.txt > ${DATA_FOLDER}/summary_i9_14900K.txt

cat ${DATA_FOLDER}/agg_A76.txt > ${DATA_FOLDER}/agg_all.txt
tail -n +2  ${DATA_FOLDER}/agg_Cortex_X3.txt >> ${DATA_FOLDER}/agg_all.txt
tail -n +2  ${DATA_FOLDER}/agg_U9_285K.txt  >> ${DATA_FOLDER}/agg_all.txt
tail -n +2  ${DATA_FOLDER}/agg_i9_14900K.txt  >> ${DATA_FOLDER}/agg_all.txt
tail -n +2  ${DATA_FOLDER}/agg_AMD_7950X.txt  >> ${DATA_FOLDER}/agg_all.txt


python3 create_graphs.py ${DATA_FOLDER}/agg_all.txt ${GRAPH_FOLDER}
