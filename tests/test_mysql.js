import { Console } from 'console';
import { exit } from 'std';
import { abbreviate, startInteractive, randStr } from 'util';
import { MySQL, MySQLResult } from 'mysql';
import extendArray from '../lib/extendArray.js';

extendArray();

('use strict');
('use math');

let resultNum = 0;

const result = r => {
  let prop = 'result' + ++resultNum;
  globalThis[prop] = r;
  console.log(/*'globalThis.' +*/ prop + ' =', r);
  return r;
};

async function main(...args) {
  globalThis.console = new Console({
    inspectOptions: {
      compact: 1,
      customInspect: true,
      showHidden: false,
      hideKeys: ['query']
    }
  });

  Object.assign(globalThis, { MySQL, MySQLResult });

  let my = (globalThis.my = new MySQL());

  my.resultType |= MySQL.RESULT_OBJECT;

  my.setOption(MySQL.OPT_NONBLOCK, true);

  console.log('2: my.getOption(OPT_NONBLOCK) =', my.getOption(MySQL.OPT_NONBLOCK));

  console.log('my.connect() =', await my.connect('192.168.178.23', 'roman', 'r4eHuJ', 'web'));

  let i;

  let q = (globalThis.q = async s => (
    console.log(`q('\x1b[0;32m${abbreviate(s, 1000)}'\x1b[0m)`),
    result(
      await my.query(s).then(
        val => result(val),
        err => {
          console.log('query error:', err);
          return null;
        }
      )
    )
  ));

  i = 0;
  let res = await q(`SELECT * FROM users LIMIT 0,10;`);

  console.log(`res =`, res);

  /*  await q(`CREATE DATABASE IF NOT EXISTS blah;`);
  await q(`USE blah;`);

  await q(
    `CREATE TABLE IF NOT EXISTS article ( id int unsigned NOT NULL auto_increment, title char(64) NOT NULL DEFAULT '', text TEXT NOT NULL DEFAULT '',  PRIMARY KEY (id)) CHARACTER SET utf8`
  );
*/

  for await(let row of res) {
    console.log(`row[${i++}] =`, row);
  }
  let users = [
    ['roman', 'r4eHuJ'],
    ['root', 'tD51o7xf']
  ];

  res = await q(
    `INSERT INTO users (username,password) VALUES ${users.map(cols => `(${MySQL.valueString(...cols)})`).join(', ')};`
  );
  console.log('res =', res);

  let affected;
  console.log('affected =', (affected = my.affectedRows));

  let id = my.insertId;
  console.log('id =', id);

  res = await q(`SELECT id FROM users WHERE username IN ('root','roman');`);
  console.log('res =', res);
  let ids = [];
  for await(let { id } of res) ids.push(id);

  for(let id of ids) {
    let newres = await q(`INSERT INTO sessions (cookie, user_id) VALUES ('${randStr(64)}', ${id});`);
    console.log('newres =', newres);
  }

  console.log('affected =', (affected = my.affectedRows));

  id = my.insertId;
  console.log('id =', id);

  i = 0;
  my.resultType &= ~(MySQL.RESULT_TABLENAME | MySQL.RESULT_OBJECT);

  res = await q(`SELECT * FROM sessions INNER JOIN users ON sessions.user_id=users.id;`);

  for await(let row of res) console.log(`session[${i++}] =`, row);

  i = 0;
  let rows = (globalThis.rows = []);

  my.resultType &= ~MySQL.RESULT_OBJECT;
  res = await q(`SELECT * FROM users ORDER BY id DESC LIMIT 0,10;`);
  for await(let row of res) {
    console.log(`row[${i++}] =`, console.config({ compact: 1 }), row);

    rows.unshift(row);
  }

  async function* showFields(table) {
    my.resultType &= ~MySQL.RESULT_OBJECT;
    let res = await q(`SHOW FIELDS FROM ${table}`);

    for await(let field of res) {
      let name = field['COLUMNS.Field'] ?? field['Field'] ?? field[0];
      let type = field[1];

      //console.log('field', { name, type });
      yield name;
    }
    // my.resultType |= MySQL.RESULT_OBJECT;
  }

  let fieldNames = (globalThis.fields = []);
  for await(let field of await showFields('sessions')) fieldNames.push(field);

  const rowValues = row => row.map(s => MySQL.valueString(s));
  const rowString = row => MySQL.valueString(...row);

  function makeInsertQuery(table, fields, data = {}) {
    return `INSERT INTO ${table} (${fields.map(f => '`' + f + '`').join(',')}) VALUES (${rowString(data)});`;
  }

  console.log('fieldNames', fieldNames);
  let myrow = [randStr(64), 1, JSON.stringify({ data: 'blah' })]; //Array.isArray(rows[0]) ? rows[0] : fieldNames.map(n => rows[0][n]);
  console.log('myrow', myrow);

  let insert = (globalThis.insert = makeInsertQuery('sessions', fieldNames.slice(1, -2), myrow.slice(0)));

  console.log('insert', insert);

  for await(let row of await q(`SELECT id,username FROM users ORDER BY created DESC LIMIT 0,10;`))
    console.log(`user[${i++}] =`, row);

  await q(insert);
  console.log('affected =', (affected = my.affectedRows));

  res = await q(`SELECT last_insert_id();`);

  console.log('res =', res);
  let iter = (globalThis.iter = res[Symbol.asyncIterator]());

  console.log('iter =', iter);
  let item = await iter.next();

  console.log('await iter.next() =', item);
  let row;
  console.log('item.value =', (row = globalThis.row = item.value));
  console.log('row[0] =', row[0]);

  console.log('id =', (id = my.insertId));

  //startInteractive();
  // os.kill(process.pid, os.SIGUSR1);
}

try {
  main(...scriptArgs.slice(1)).catch(err => console.log(`FAIL: ${err.message}\n${err.stack}`));
} catch(error) {
  console.log(`FAIL: ${error.message}\n${error.stack}`);
  exit(1);
} finally {
  console.log('SUCCESS');
}
