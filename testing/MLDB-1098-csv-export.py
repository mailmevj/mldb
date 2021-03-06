# -*- coding: utf-8 -*-

# This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

#
# MLDB-1089-csv-export.py
# Mich, 2015-11-16
# Copyright (c) 2015 Datacratic Inc. All rights reserved.
#

import tempfile
import codecs
import unittest

if False:
    mldb_wrapper = None
mldb = mldb_wrapper.wrap(mldb) # noqa


class CsvExportTest(unittest.TestCase):

    def assert_file_content(self, filename, lines_expect):
        f = codecs.open(filename, 'rt', 'utf8')
        for index, expect in enumerate(lines_expect):
            line = f.readline()[:-1]
            self.assertEqual(line, expect)

    def test_can_log_utf8(self):
        mldb.log("MLDB can log utf 8 text")
        mldb.log("Ǆώύψ")
        mldb.log("ăØÆÅ")

    def test_export_csv_with_utf8_roundtrip(self):
        mldb.put('/v1/datasets/myDataset', {
            'type' : 'sparse.mutable'
        })

        mldb.post('/v1/datasets/myDataset/rows', {
            'rowName' : 'ascii row',
            'columns' : [
                ['colA', 1, 0],
                ["colB", 2, 0]
            ]
        })

        mldb.post('/v1/datasets/myDataset/rows', {
            'rowName' : 'utf8 row',
            'columns' : [
                ['colA', 'Ǆώύψ', 0],
                ["colB", 'ăØÆÅ', 0]
            ]
        })

        mldb.post('/v1/datasets/myDataset/commit')

        res = mldb.get('/v1/query', q='SELECT * FROM myDataset')
        mldb.log(res)

        tmp_file = tempfile.NamedTemporaryFile(dir='build/x86_64/tmp')

        res = mldb.put('/v1/procedures/export', {
            'type' : 'export.csv',
            'params' : {
                'exportData' :
                    'select rowName() as rowName, colA, colB from myDataset',
                'dataFileUrl' : 'file://' + tmp_file.name
            }
        })

        mldb.post('/v1/procedures/export/runs', {})

        lines_expect = ['rowName,colA,colB',
                        u'utf8 row,Ǆώύψ,ăØÆÅ',
                        'ascii row,1,2'
                        ]
        self.assert_file_content(tmp_file.name, lines_expect)

        # import it
        csv_conf = {
            "type": "import.text",
            "params": {
                'dataFileUrl' : 'file://' + tmp_file.name,
                "outputDataset": {
                    "id": "myDataset2",
                },
                "runOnCreation": True,
                "named" : "rowName"
            }
        }
        mldb.put("/v1/procedures/csv_proc", csv_conf) 

        # export it (end of roundtrip)
        tmp_file2 = tempfile.NamedTemporaryFile(dir='build/x86_64/tmp')
        mldb.put('/v1/procedures/export2', {
            'type' : 'export.csv',
            'params' : {
                'exportData' :
                    'select rowName() as rowName, colA, colB from myDataset2',
                'dataFileUrl' : 'file://' + tmp_file2.name
            }
        })

        mldb.post('/v1/procedures/export2/runs', {})

        self.assert_file_content(tmp_file2.name, lines_expect)

    def test_quotechar_delimiter_noheader(self):
        tmp_file = tempfile.NamedTemporaryFile(dir='build/x86_64/tmp')
        mldb.put('/v1/procedures/export3', {
            'type' : 'export.csv',
            'params' : {
                'exportData' :
                    'select rowName() as rowName, colA, colB from myDataset2',
                'dataFileUrl' : 'file://' + tmp_file.name,
                'headers' : False,
                'quoteChar' : 'o',
                'delimiter' : ';'
            }
        })

        mldb.post('/v1/procedures/export3/runs')

        lines_expect = [u'outf8 roowo;Ǆώύψ;ăØÆÅ',
                        'oascii roowo;1;2']
        self.assert_file_content(tmp_file.name, lines_expect)

    def test_bad_target(self):
        with self.assertRaises(mldb_wrapper.ResponseException):
            mldb.put('/v1/procedures/export4', {
                'type' : 'export.csv',
                'params' : {
                    'exportData' :
                        'select rowName() as rowName, colA, colB '
                        'from myDataset2',
                    'dataFileUrl' : 'space',
                    'headers' : False,
                    'quoteChar' : 'o',
                    'delimiter' : ';'
                }
            })

if __name__ == '__main__':
    mldb.run_tests()
