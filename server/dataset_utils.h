/** dataset_utils.h                                 -*- C++ -*-
    Rémi Attab, 29 Apr 2015
    Copyright (c) 2015 Datacratic.  All rights reserved.

    This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.
*/

#pragma once

#include "mldb/core/dataset.h"

namespace Datacratic {
namespace MLDB {

/******************************************************************************/
/* MERGED MATRIX VIEW                                                         */
/******************************************************************************/

/** Creates a merged view of the provided matrix views.

    Assumes that the underlying indexes are mutable and will therefore perform a
    full table scan for operations that operates on multiple rows or columns.
 */
struct MergedMatrixView : public MatrixView
{
    MergedMatrixView(std::vector< std::shared_ptr<MatrixView> > views);

    std::vector<RowName>
    getRowNames(ssize_t start = 0, ssize_t limit = -1) const;

    std::vector<RowHash>
    getRowHashes(ssize_t start = 0, ssize_t limit = -1) const;

    size_t getRowCount() const;
    bool knownRow(const RowName & row) const;
    MatrixNamedRow getRow(const RowName & row) const;

    bool knownColumn(const ColumnName & column) const;
    std::vector<ColumnName> getColumnNames() const;
    size_t getColumnCount() const;

private:
    std::vector< std::shared_ptr<MatrixView> > views;
};


/******************************************************************************/
/* MERGED COLUMN INDEXES                                                      */
/******************************************************************************/

/** Creates a merged "index" of the provided column indexes.

    Assumes that the underlying indexes are mutable and will therefore perform a
    full table scan for operations that operates on multiple columns.
 */
struct MergedColumnIndex : public ColumnIndex
{
    MergedColumnIndex(std::vector< std::shared_ptr<ColumnIndex> > indexes);

    MatrixColumn getColumn(const ColumnName & column) const;
    bool knownColumn(const ColumnName & column) const;
    std::vector<ColumnName> getColumnNames() const;

private:
    std::vector< std::shared_ptr<ColumnIndex> > indexes;
};

} // namespace MLDB
} // namespace Datacratic
