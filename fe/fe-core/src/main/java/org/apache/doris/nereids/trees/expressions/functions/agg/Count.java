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

package org.apache.doris.nereids.trees.expressions.functions.agg;

import org.apache.doris.nereids.exceptions.UnboundException;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.functions.AlwaysNotNullable;
import org.apache.doris.nereids.trees.expressions.shape.UnaryExpression;
import org.apache.doris.nereids.trees.expressions.visitor.ExpressionVisitor;
import org.apache.doris.nereids.types.BigIntType;
import org.apache.doris.nereids.types.DataType;

import com.google.common.base.Preconditions;

import java.util.List;
import java.util.stream.Collectors;

/** count agg function. */
public class Count extends AggregateFunction implements UnaryExpression, AlwaysNotNullable {

    private final boolean isStar;

    public Count() {
        super("count");
        this.isStar = true;
    }

    public Count(Expression child, boolean isDistinct) {
        super("count", isDistinct, child);
        this.isStar = false;
    }

    public boolean isStar() {
        return isStar;
    }

    @Override
    public DataType getDataType() {
        return BigIntType.INSTANCE;
    }

    @Override
    public Expression withChildren(List<Expression> children) {
        Preconditions.checkArgument(children.size() == 0 || children.size() == 1);
        if (children.size() == 0) {
            return new Count();
        }
        return new Count(children.get(0), isDistinct());
    }

    @Override
    public DataType getIntermediateType() {
        return getDataType();
    }

    @Override
    public String toSql() throws UnboundException {
        if (isStar) {
            return "count(*)";
        }
        String args = children()
                .stream()
                .map(Expression::toSql)
                .collect(Collectors.joining(", "));
        if (isDistinct()) {
            return "count(distinct " + args + ")";
        }
        return "count(" + args + ")";
    }

    @Override
    public String toString() {
        if (isStar) {
            return "count(*)";
        }
        String args = children()
                .stream()
                .map(Expression::toString)
                .collect(Collectors.joining(", "));
        if (isDistinct()) {
            return "count(distinct " + args + ")";
        }
        return "count(" + args + ")";
    }

    @Override
    public <R, C> R accept(ExpressionVisitor<R, C> visitor, C context) {
        return visitor.visitCount(this, context);
    }
}
