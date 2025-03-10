// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.trees.expressions;

import org.apache.doris.nereids.exceptions.UnboundException;
import org.apache.doris.nereids.trees.expressions.visitor.ExpressionVisitor;
import org.apache.doris.nereids.types.BooleanType;
import org.apache.doris.nereids.types.DataType;

import java.util.Objects;

/**
 * Comparison predicate expression.
 * Such as: "=", "<", "<=", ">", ">=", "<=>"
 */
public abstract class ComparisonPredicate extends Expression implements BinaryExpression {

    protected final String symbol;

    /**
     * Constructor of ComparisonPredicate.
     *
     * @param nodeType node type of expression
     * @param left     left child of comparison predicate
     * @param right    right child of comparison predicate
     */
    public ComparisonPredicate(ExpressionType nodeType, Expression left, Expression right, String symbol) {
        super(nodeType, left, right);
        this.symbol = symbol;
    }

    @Override
    public DataType getDataType() throws UnboundException {
        return BooleanType.INSTANCE;
    }

    @Override
    public boolean nullable() throws UnboundException {
        return left().nullable() || right().nullable();
    }

    @Override
    public String toSql() {
        return "(" + left().toSql() + ' ' + symbol + ' ' + right().toSql() + ")";
    }

    public <R, C> R accept(ExpressionVisitor<R, C> visitor, C context) {
        return visitor.visitComparisonPredicate(this, context);
    }

    @Override
    public int hashCode() {
        return Objects.hash(type, left(), right());
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }
        ComparisonPredicate other = (ComparisonPredicate) o;
        return (type == other.getType()) && Objects.equals(left(), other.left())
                && Objects.equals(right(), other.right());
    }
}
