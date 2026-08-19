function hash32(value) {
  let hash = 0x811c9dc5;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 0x01000193);
  }
  return hash >>> 0;
}

export function embedText(text, dimensions) {
  if (typeof text !== "string" || text.trim().length === 0) {
    throw new TypeError("text must be a non-empty string");
  }
  if (!Number.isInteger(dimensions) || dimensions <= 0 || dimensions > 65_536) {
    throw new RangeError("embedding dimensions must be between 1 and 65536");
  }

  const normalized = text.toLocaleLowerCase("en-US").normalize("NFKC");
  const tokens = normalized.match(/[\p{L}\p{N}]+/gu) ?? [];
  if (tokens.length === 0) throw new TypeError("text has no indexable tokens");
  const vector = new Float32Array(dimensions);
  const features = [...tokens];
  for (let index = 1; index < tokens.length; index += 1) {
    features.push(`${tokens[index - 1]}::${tokens[index]}`);
  }
  for (const token of tokens) {
    if (token.length >= 3) {
      for (let index = 0; index + 3 <= token.length; index += 1) {
        features.push(`#${token.slice(index, index + 3)}`);
      }
    }
  }
  for (const feature of features) {
    const hash = hash32(feature);
    const slot = hash % dimensions;
    const sign = (hash & 0x80000000) === 0 ? 1 : -1;
    vector[slot] += sign;
  }
  let norm = 0;
  for (const value of vector) norm += value * value;
  norm = Math.sqrt(norm);
  if (norm === 0) throw new TypeError("text produced an empty embedding");
  for (let index = 0; index < vector.length; index += 1) vector[index] /= norm;
  return Array.from(vector);
}

export function encodeFp32(values, dimensions) {
  if (!Array.isArray(values) || values.length !== dimensions) {
    throw new RangeError(`vector must contain exactly ${dimensions} values`);
  }
  const output = Buffer.allocUnsafe(dimensions * 4);
  values.forEach((value, index) => {
    if (typeof value !== "number" || !Number.isFinite(value)) {
      throw new TypeError(`vector value ${index} is not finite`);
    }
    output.writeFloatLE(value, index * 4);
  });
  return output;
}

export function seededVector(dimensions, seed, cluster = 0) {
  let state = (seed ^ Math.imul(cluster + 1, 0x9e3779b1)) >>> 0;
  const vector = new Array(dimensions);
  let norm = 0;
  for (let index = 0; index < dimensions; index += 1) {
    state = (Math.imul(state, 1_664_525) + 1_013_904_223) >>> 0;
    const random = state / 0xffffffff;
    const centroid = index % 16 === cluster % 16 ? 0.8 : 0;
    const value = centroid + (random - 0.5) * 0.35;
    vector[index] = value;
    norm += value * value;
  }
  norm = Math.sqrt(norm) || 1;
  return vector.map((value) => value / norm);
}
