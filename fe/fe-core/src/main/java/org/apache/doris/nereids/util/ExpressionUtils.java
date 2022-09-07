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

package org.apache.doris.nereids.util;

import org.apache.doris.nereids.trees.expressions.And;
import org.apache.doris.nereids.trees.expressions.Cast;
import org.apache.doris.nereids.trees.expressions.CompoundPredicate;
import org.apache.doris.nereids.trees.expressions.ExprId;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.Or;
import org.apache.doris.nereids.trees.expressions.SlotReference;
import org.apache.doris.nereids.trees.expressions.literal.BooleanLiteral;
import org.apache.doris.nereids.trees.expressions.visitor.DefaultExpressionRewriter;

import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;
import com.google.common.collect.Sets;

import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;

/**
 * Expression rewrite helper class.
 */
public class ExpressionUtils {

    public static List<Expression> extractConjunction(Expression expr) {
        return extract(And.class, expr);
    }

    public static List<Expression> extractDisjunction(Expression expr) {
        return extract(Or.class, expr);
    }

    /**
     * Split predicates with `And/Or` form recursively.
     * Some examples for `And`:
     * <p>
     * a and b -> a, b
     * (a and b) and c -> a, b, c
     * (a or b) and (c and d) -> (a or b), c , d
     * <p>
     * Stop recursion when meeting `Or`, so this func will ignore `And` inside `Or`.
     * Warning examples:
     * (a and b) or c -> (a and b) or c
     */
    public static List<Expression> extract(CompoundPredicate expr) {
        return extract(expr.getClass(), expr);
    }

    private static List<Expression> extract(Class<? extends Expression> type, Expression expr) {
        List<Expression> result = Lists.newArrayList();
        extract(type, expr, result);
        return result;
    }

    private static void extract(Class<? extends Expression> type, Expression expr, List<Expression> result) {
        if (type.isInstance(expr)) {
            CompoundPredicate predicate = (CompoundPredicate) expr;
            extract(type, predicate.left(), result);
            extract(type, predicate.right(), result);
        } else {
            result.add(expr);
        }
    }

    public static Expression and(List<Expression> expressions) {
        return combine(And.class, expressions);
    }

    public static Expression and(Expression... expressions) {
        return combine(And.class, Lists.newArrayList(expressions));
    }

    public static Expression or(Expression... expressions) {
        return combine(Or.class, Lists.newArrayList(expressions));
    }

    public static Expression or(List<Expression> expressions) {
        return combine(Or.class, expressions);
    }

    /**
     * Use AND/OR to combine expressions together.
     */
    public static Expression combine(Class<? extends Expression> type, List<Expression> expressions) {
        /*
         *             (AB) (CD) E   ((AB)(CD))  E     (((AB)(CD))E)
         *               ▲   ▲   ▲       ▲       ▲          ▲
         *               │   │   │       │       │          │
         * A B C D E ──► A B C D E ──► (AB) (CD) E ──► ((AB)(CD)) E ──► (((AB)(CD))E)
         */
        Preconditions.checkArgument(type == And.class || type == Or.class);
        Objects.requireNonNull(expressions, "expressions is null");

        Expression shortCircuit = (type == And.class ? BooleanLiteral.FALSE : BooleanLiteral.TRUE);
        Expression skip = (type == And.class ? BooleanLiteral.TRUE : BooleanLiteral.FALSE);
        Set<Expression> distinctExpressions = Sets.newLinkedHashSetWithExpectedSize(expressions.size());
        for (Expression expression : expressions) {
            if (expression.equals(shortCircuit)) {
                return shortCircuit;
            } else if (!expression.equals(skip)) {
                distinctExpressions.add(expression);
            }
        }

        return distinctExpressions.stream()
                .reduce(type == And.class ? And::new : Or::new)
                .orElse(new BooleanLiteral(type == And.class));
    }

    /**
     * Check whether the input expression is a {@link org.apache.doris.nereids.trees.expressions.Slot}
     * or at least one {@link Cast} on a {@link org.apache.doris.nereids.trees.expressions.Slot}
     * <p>
     * for example:
     * - SlotReference to a column:
     * col
     * - Cast on SlotReference:
     * cast(int_col as string)
     * cast(cast(int_col as long) as string)
     *
     * @param expr input expression
     * @return Return Optional[ExprId] of underlying slot reference if input expression is a slot or cast on slot.
     *         Otherwise, return empty optional result.
     */
    public static Optional<ExprId> isSlotOrCastOnSlot(Expression expr) {
        while (expr instanceof Cast) {
            expr = expr.child(0);
        }

        if (expr instanceof SlotReference) {
            return Optional.of(((SlotReference) expr).getExprId());
        } else {
            return Optional.empty();
        }
    }

    /**
     * Replace expression node in the expression tree by `replaceMap` in top-down manner.
     * For example.
     * <pre>
     * input expression: a > 1
     * replaceMap: a -> b + c
     *
     * output:
     * b + c > 1
     * </pre>
     */
    public static Expression replace(Expression expr, Map<Expression, Expression> replaceMap) {
        return expr.accept(ExpressionReplacer.INSTANCE, replaceMap);
    }

    private static class ExpressionReplacer extends DefaultExpressionRewriter<Map<Expression, Expression>> {
        public static final ExpressionReplacer INSTANCE = new ExpressionReplacer();

        private ExpressionReplacer() {
        }

        @Override
        public Expression visit(Expression expr, Map<Expression, Expression> replaceMap) {
            if (replaceMap.containsKey(expr)) {
                return replaceMap.get(expr);
            }
            return super.visit(expr, replaceMap);
        }
    }
}
