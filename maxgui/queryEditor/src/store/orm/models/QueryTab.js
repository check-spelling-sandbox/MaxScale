/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-11-16
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
import Extender from '@queryEditorSrc/store/orm/Extender'
import { ORM_PERSISTENT_ENTITIES } from '@queryEditorSrc/store/config'
import { t } from 'typy'
import { uuidv1 } from '@share/utils/helpers'
import queryHelper from '@queryEditorSrc/store/queryHelper'
import QueryResult from './QueryResult'
import QueryConn from './QueryConn'
import Editor from './Editor'
import QueryTabMem from './QueryTabMem'

export default class QueryTab extends Extender {
    static entity = ORM_PERSISTENT_ENTITIES.QUERY_TABS

    static state() {
        return {
            active_query_tab_map: {}, // Persistence, key is worksheet_id, value is entity id
        }
    }

    /**
     * If a record in the parent table (queryTab) is deleted, then the corresponding records in the child
     * tables (queryResult, queryConn, editor) will automatically be deleted
     * @param {String|Function} payload - either a queryTab id or a callback function that return Boolean (filter)
     */
    static cascadeDelete(payload) {
        const modelIds = queryHelper.filterEntity(QueryTab, payload).map(model => model.id)
        modelIds.forEach(id => {
            QueryTab.delete(t => t.id === id) // delete itself
            QueryTabMem.delete(m => m.query_tab_id === id)

            // delete records in the child tables
            Editor.delete(e => e.query_tab_id === id)
            QueryResult.delete(r => r.query_tab_id === id)
            QueryConn.delete(c => c.query_tab_id === id)
        })
    }

    /**
     * @returns {Object} - return fields that are not key, relational fields
     */
    static getNonKeyFields() {
        return { name: this.string('Query Tab 1'), count: this.number(1) }
    }

    /**
     * Refresh non-key and non-relational fields of an entity and its relations
     * @param {String|Function} payload - either a QueryTab id or a callback function that return Boolean (filter)
     */
    static cascadeRefresh(payload) {
        const models = queryHelper.filterEntity(QueryTab, payload)
        models.forEach(model => {
            const target = QueryTab.query()
                .with('editor') // get editor relational field
                .whereId(model.id)
                .first()
            if (target) {
                //----------------------- refresh itself --------------------------
                QueryTab.update({
                    where: model.id,
                    data: {
                        // refresh the name if the editor doesn't have a blob_file
                        name: t(target, 'editor.blob_file').isNull
                            ? `Query Tab ${target.count}`
                            : target.name,
                    },
                })
                // refresh its relations
                QueryTabMem.refresh(m => m.query_tab_id === model.id)
                // keep query_txt and blob_file data even after refresh all fields
                Editor.refresh(e => e.query_tab_id === model.id, ['blob_file', 'query_txt'])
                QueryResult.refresh(r => r.query_tab_id === model.id)
                QueryConn.refresh(c => c.query_tab_id === model.id)
            }
        })
    }

    static fields() {
        return {
            id: this.uid(() => uuidv1()),
            ...this.getNonKeyFields(),
            //FK
            worksheet_id: this.attr(null),
            // relationship fields
            editor: this.hasOne(Editor, 'query_tab_id'),
            queryResult: this.hasOne(QueryResult, 'query_tab_id'),
            queryConn: this.hasOne(QueryConn, 'query_tab_id'),
            queryTabMem: this.hasOne(QueryTabMem, 'query_tab_id'),
        }
    }
}
