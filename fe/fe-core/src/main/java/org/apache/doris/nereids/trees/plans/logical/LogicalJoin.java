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

package org.apache.doris.nereids.trees.plans.logical;

import org.apache.doris.nereids.memo.GroupExpression;
import org.apache.doris.nereids.properties.LogicalProperties;
import org.apache.doris.nereids.rules.exploration.JoinReorderContext;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.plans.JoinType;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.PlanType;
import org.apache.doris.nereids.trees.plans.visitor.PlanVisitor;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;

import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.stream.Collectors;

/**
 * Logical join plan.
 */
public class LogicalJoin<LEFT_CHILD_TYPE extends Plan, RIGHT_CHILD_TYPE extends Plan>
        extends LogicalBinary<LEFT_CHILD_TYPE, RIGHT_CHILD_TYPE> {

    private final JoinType joinType;
    private final Optional<Expression> condition;

    // Use for top-to-down join reorder
    private final JoinReorderContext joinReorderContext = new JoinReorderContext();

    /**
     * Constructor for LogicalJoinPlan.
     *
     * @param joinType logical type for join
     */
    public LogicalJoin(JoinType joinType, LEFT_CHILD_TYPE leftChild, RIGHT_CHILD_TYPE rightChild) {
        this(joinType, Optional.empty(), Optional.empty(), Optional.empty(), leftChild, rightChild);
    }

    public LogicalJoin(JoinType joinType, Optional<Expression> condition,
            LEFT_CHILD_TYPE leftChild, RIGHT_CHILD_TYPE rightChild) {
        this(joinType, condition, Optional.empty(), Optional.empty(), leftChild, rightChild);
    }

    /**
     * Constructor for LogicalJoinPlan.
     *
     * @param joinType logical type for join
     * @param condition on clause for join node
     */
    public LogicalJoin(JoinType joinType, Optional<Expression> condition,
            Optional<GroupExpression> groupExpression, Optional<LogicalProperties> logicalProperties,
            LEFT_CHILD_TYPE leftChild, RIGHT_CHILD_TYPE rightChild) {
        super(PlanType.LOGICAL_JOIN, groupExpression, logicalProperties, leftChild, rightChild);
        this.joinType = Objects.requireNonNull(joinType, "joinType can not be null");
        this.condition = Objects.requireNonNull(condition, "condition can not be null");
    }

    public Optional<Expression> getCondition() {
        return condition;
    }

    public JoinType getJoinType() {
        return joinType;
    }

    @Override
    public List<Slot> computeOutput(Plan leftInput, Plan rightInput) {

        List<Slot> newLeftOutput = leftInput.getOutput().stream().map(o -> o.withNullable(true))
                .collect(Collectors.toList());

        List<Slot> newRightOutput = rightInput.getOutput().stream().map(o -> o.withNullable(true))
                .collect(Collectors.toList());

        switch (joinType) {
            case LEFT_SEMI_JOIN:
            case LEFT_ANTI_JOIN:
                return ImmutableList.copyOf(leftInput.getOutput());
            case RIGHT_SEMI_JOIN:
            case RIGHT_ANTI_JOIN:
                return ImmutableList.copyOf(rightInput.getOutput());
            case LEFT_OUTER_JOIN:
                return ImmutableList.<Slot>builder()
                        .addAll(leftInput.getOutput())
                        .addAll(newRightOutput)
                        .build();
            case RIGHT_OUTER_JOIN:
                return ImmutableList.<Slot>builder()
                        .addAll(newLeftOutput)
                        .addAll(rightInput.getOutput())
                        .build();
            case FULL_OUTER_JOIN:
                return ImmutableList.<Slot>builder()
                        .addAll(newLeftOutput)
                        .addAll(newRightOutput)
                        .build();
            default:
                return ImmutableList.<Slot>builder()
                        .addAll(leftInput.getOutput())
                        .addAll(rightInput.getOutput())
                        .build();
        }
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder("LogicalJoin (").append(joinType);
        condition.ifPresent(expression -> sb.append(", ").append(expression));
        return sb.append(")").toString();
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }
        LogicalJoin that = (LogicalJoin) o;
        return joinType == that.joinType && Objects.equals(condition, that.condition);
    }

    @Override
    public int hashCode() {
        return Objects.hash(joinType, condition);
    }

    @Override
    public <R, C> R accept(PlanVisitor<R, C> visitor, C context) {
        return visitor.visitLogicalJoin((LogicalJoin<Plan, Plan>) this, context);
    }

    @Override
    public List<Expression> getExpressions() {
        return condition.<List<Expression>>map(ImmutableList::of).orElseGet(ImmutableList::of);
    }

    public JoinReorderContext getJoinReorderContext() {
        return joinReorderContext;
    }

    @Override
    public LogicalBinary<Plan, Plan> withChildren(List<Plan> children) {
        Preconditions.checkArgument(children.size() == 2);
        return new LogicalJoin<>(joinType, condition, children.get(0), children.get(1));
    }

    @Override
    public Plan withGroupExpression(Optional<GroupExpression> groupExpression) {
        return new LogicalJoin<>(joinType, condition, groupExpression,
                Optional.of(logicalProperties), left(), right());
    }

    @Override
    public Plan withLogicalProperties(Optional<LogicalProperties> logicalProperties) {
        return new LogicalJoin<>(joinType, condition, Optional.empty(), logicalProperties, left(), right());
    }
}
