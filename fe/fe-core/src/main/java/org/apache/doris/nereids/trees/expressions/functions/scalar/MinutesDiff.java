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

package org.apache.doris.nereids.trees.expressions.functions.scalar;

import org.apache.doris.catalog.FunctionSignature;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.functions.AlwaysNullable;
import org.apache.doris.nereids.trees.expressions.functions.ExplicitlyCastableSignature;
import org.apache.doris.nereids.trees.expressions.shape.BinaryExpression;
import org.apache.doris.nereids.trees.expressions.visitor.ExpressionVisitor;
import org.apache.doris.nereids.types.BigIntType;
import org.apache.doris.nereids.types.DateTimeType;
import org.apache.doris.nereids.types.DateTimeV2Type;
import org.apache.doris.nereids.types.DateV2Type;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;

import java.util.List;

/**
 * ScalarFunction 'minutes_diff'. This class is generated by GenerateFunction.
 */
public class MinutesDiff extends ScalarFunction
        implements BinaryExpression, ExplicitlyCastableSignature, AlwaysNullable {

    public static final List<FunctionSignature> SIGNATURES = ImmutableList.of(
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateTimeType.INSTANCE, DateTimeType.INSTANCE),
            FunctionSignature.ret(BigIntType.INSTANCE)
                    .args(DateTimeV2Type.SYSTEM_DEFAULT, DateTimeV2Type.SYSTEM_DEFAULT),
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateV2Type.INSTANCE, DateTimeV2Type.SYSTEM_DEFAULT),
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateTimeV2Type.SYSTEM_DEFAULT, DateV2Type.INSTANCE),
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateV2Type.INSTANCE, DateV2Type.INSTANCE),
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateV2Type.INSTANCE, DateTimeType.INSTANCE),
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateTimeType.INSTANCE, DateV2Type.INSTANCE),
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateTimeV2Type.SYSTEM_DEFAULT, DateTimeType.INSTANCE),
            FunctionSignature.ret(BigIntType.INSTANCE).args(DateTimeType.INSTANCE, DateTimeV2Type.SYSTEM_DEFAULT)
    );

    /**
     * constructor with 2 arguments.
     */
    public MinutesDiff(Expression arg0, Expression arg1) {
        super("minutes_diff", arg0, arg1);
    }

    /**
     * withChildren.
     */
    @Override
    public MinutesDiff withChildren(List<Expression> children) {
        Preconditions.checkArgument(children.size() == 2);
        return new MinutesDiff(children.get(0), children.get(1));
    }

    @Override
    public List<FunctionSignature> getSignatures() {
        return SIGNATURES;
    }

    @Override
    public <R, C> R accept(ExpressionVisitor<R, C> visitor, C context) {
        return visitor.visitMinutesDiff(this, context);
    }
}
