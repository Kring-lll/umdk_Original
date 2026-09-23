/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: TF-IDF CatERR classifier test
 * Create: 2026-08-31
 */

// Package gs is the global scheduler for gateway.
package gs

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

func TestCatErrClassifierNotLoaded(t *testing.T) {
	c := NewCatErrClassifier()
	if c.IsLoaded() {
		t.Error("New classifier should not be loaded")
	}
	if c.Classify("hello") != "" {
		t.Error("Classify on unloaded classifier should return empty string")
	}
	if labels := c.Labels(); labels != nil {
		t.Errorf("Labels on unloaded classifier = %v, want nil", labels)
	}
}

func TestCatErrClassifierLoadVocab(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["hello","world","code"],"GIdf":{"hello":1.5,"world":2.0,"code":1.0}}`), 0644)

	c := NewCatErrClassifier()
	if err := c.LoadVocab(vocabPath); err != nil {
		t.Fatalf("LoadVocab failed: %v", err)
	}
}

func TestCatErrClassifierLoadVocabInvalid(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":[],"GIdf":{}}`), 0644)

	c := NewCatErrClassifier()
	if err := c.LoadVocab(vocabPath); err == nil {
		t.Error("LoadVocab with empty data should return error")
	}
}

func TestCatErrClassifierLoadVocabNonExistent(t *testing.T) {
	c := NewCatErrClassifier()
	if err := c.LoadVocab("/nonexistent/path.json"); err == nil {
		t.Error("LoadVocab on non-existent file should return error")
	}
}

func TestCatErrClassifierLoadCentroids(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["hello","world"],"GIdf":{"hello":1.0,"world":1.0}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.0],[0.0,1.0]],"labels":["cat_a","cat_b"]}`), 0644)

	c := NewCatErrClassifier()
	if err := c.LoadVocab(vocabPath); err != nil {
		t.Fatalf("LoadVocab failed: %v", err)
	}
	if err := c.LoadCentroids(centroidsPath); err != nil {
		t.Fatalf("LoadCentroids failed: %v", err)
	}
	if !c.IsLoaded() {
		t.Error("Classifier should be loaded after LoadVocab + LoadCentroids")
	}
	labels := c.Labels()
	if len(labels) != 2 || labels[0] != "cat_a" || labels[1] != "cat_b" {
		t.Errorf("Labels = %v, want [cat_a, cat_b]", labels)
	}
}

func TestCatErrClassifierLoadCentroidsMismatch(t *testing.T) {
	tmpDir := t.TempDir()
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0],[0.0],[1.0]],"labels":["cat_a","cat_b"]}`), 0644)

	c := NewCatErrClassifier()
	if err := c.LoadCentroids(centroidsPath); err == nil {
		t.Error("LoadCentroids with mismatched labels/centroids should return error")
	}
}

func TestCatErrClassifierClassify(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	// Vocab: 5 code words + 5 office words
	// Centroid 0 (code):     [1, 1, 1, 1, 1, 0, 0, 0, 0, 0] / sqrt(5)
	// Centroid 1 (office):   [0, 0, 0, 0, 0, 1, 1, 1, 1, 1] / sqrt(5)
	vocabJSON := `{"GVocab":["code","debug","python","function","sort","report","meeting","email","budget","presentation"],"GIdf":{"code":1.0,"debug":1.0,"python":1.0,"function":1.0,"sort":1.0,"report":1.0,"meeting":1.0,"email":1.0,"budget":1.0,"presentation":1.0}}`
	norm := 1.0 / 3.1622776601683795 // 1/sqrt(10), but we use 5 ones per centroid → 1/sqrt(5)
	norm = 0.4472135954999579
	centroidsJSON := fmt.Sprintf(`{"centroids":[[%f,%f,%f,%f,%f,0,0,0,0,0],[0,0,0,0,0,%f,%f,%f,%f,%f]],"labels":["code","office"]}`,
		norm, norm, norm, norm, norm, norm, norm, norm, norm, norm)

	os.WriteFile(vocabPath, []byte(vocabJSON), 0644)
	os.WriteFile(centroidsPath, []byte(centroidsJSON), 0644)

	c := NewCatErrClassifier()
	if err := c.LoadVocab(vocabPath); err != nil {
		t.Fatalf("LoadVocab failed: %v", err)
	}
	if err := c.LoadCentroids(centroidsPath); err != nil {
		t.Fatalf("LoadCentroids failed: %v", err)
	}

	// Code prompt: all code vocab words, zero office words → cosine(code centroid) = 1.0, cosine(office centroid) = 0.0
	cat := c.Classify("code debug python function sort")
	if cat != "code" {
		t.Errorf("Classify code prompt = %q, want 'code'", cat)
	}

	// Office prompt: all office vocab words → cosine(office centroid) = 1.0
	cat = c.Classify("report meeting email budget presentation")
	if cat != "office" {
		t.Errorf("Classify office prompt = %q, want 'office'", cat)
	}

	// Mixed prompt with more code words → code
	cat = c.Classify("code debug python report meeting")
	if cat != "code" {
		t.Errorf("Classify code-leaning mixed prompt = %q, want 'code'", cat)
	}

	// Mixed prompt with more office words → office
	cat = c.Classify("report meeting email budget code")
	if cat != "office" {
		t.Errorf("Classify office-leaning mixed prompt = %q, want 'office'", cat)
	}

	// Only stop words → no vocabulary hit → "" (MEJ fallback), NOT an
	// assertion of the first cluster
	cat = c.Classify("the is at which on")
	if cat != "" {
		t.Errorf("Classify stop-words-only prompt = %q, want \"\" (unclassifiable)", cat)
	}
}

func TestCatErrClassifierClassifyEmptyPrompt(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["hello","world"],"GIdf":{"hello":1.0,"world":1.0}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.0],[0.0,1.0]],"labels":["a","b"]}`), 0644)

	c := NewCatErrClassifier()
	c.LoadVocab(vocabPath)
	c.LoadCentroids(centroidsPath)

	// Empty prompt → "" (unclassifiable → MEJ fallback), not the first label
	cat := c.Classify("")
	if cat != "" {
		t.Errorf("Classify empty prompt = %q, want \"\" (unclassifiable)", cat)
	}
}

func TestTokenizePrompt(t *testing.T) {
	tokens := tokenizePrompt("Hello World 123 test")
	if len(tokens) != 3 {
		t.Errorf("tokenizePrompt('Hello World 123 test') = %v, want 3 tokens", tokens)
	}

	// Single char words should be filtered (sklearn \w\w+ = 2+ chars)
	tokens = tokenizePrompt("a b cd ef")
	if len(tokens) != 2 {
		t.Errorf("tokenizePrompt single chars = %v, want 2 tokens (cd, ef)", tokens)
	}
}

func TestTokenizePromptCJK(t *testing.T) {
	tokens := tokenizePrompt("你好世界 code")
	if len(tokens) < 4 {
		t.Errorf("tokenizePrompt CJK = %v, expected at least 4 tokens", tokens)
	}
	// Should contain individual CJK chars
	foundCJK := false
	for _, tok := range tokens {
		if tok == "你" || tok == "好" || tok == "世" || tok == "界" {
			foundCJK = true
			break
		}
	}
	if !foundCJK {
		t.Errorf("tokenizePrompt CJK = %v, expected CJK chars as individual tokens", tokens)
	}
}

func TestTokenizePromptEmpty(t *testing.T) {
	if tokens := tokenizePrompt(""); tokens != nil {
		t.Errorf("tokenizePrompt('') = %v, want nil", tokens)
	}
}

func TestBuildNgrams(t *testing.T) {
	tokens := []string{"hello", "world", "code"}
	terms := buildNgrams(tokens)
	// 3 unigrams + 2 bigrams = 5
	if len(terms) != 5 {
		t.Errorf("buildNgrams = %v (len %d), want 5 terms", terms, len(terms))
	}

	// Check bigrams
	foundBigram := false
	for _, term := range terms {
		if term == "hello world" {
			foundBigram = true
			break
		}
	}
	if !foundBigram {
		t.Errorf("buildNgrams missing bigram 'hello world': %v", terms)
	}
}

func TestBuildNgramsEmpty(t *testing.T) {
	if terms := buildNgrams(nil); terms != nil {
		t.Errorf("buildNgrams(nil) = %v, want nil", terms)
	}
}

func TestBuildNgramsSingle(t *testing.T) {
	terms := buildNgrams([]string{"only"})
	if len(terms) != 1 {
		t.Errorf("buildNgrams single token = %v, want 1 term", terms)
	}
}

func TestCosineSim(t *testing.T) {
	// Same direction → 1.0
	a := []float64{1.0, 0.0}
	b := []float64{1.0, 0.0}
	if sim := cosineSim(a, b); sim != 1.0 {
		t.Errorf("cosineSim same direction = %v, want 1.0", sim)
	}

	// Orthogonal → 0.0
	a = []float64{1.0, 0.0}
	b = []float64{0.0, 1.0}
	if sim := cosineSim(a, b); sim != 0.0 {
		t.Errorf("cosineSim orthogonal = %v, want 0.0", sim)
	}

	// Different length → 0.0
	a = []float64{1.0, 0.0}
	b = []float64{1.0}
	if sim := cosineSim(a, b); sim != 0.0 {
		t.Errorf("cosineSim different length = %v, want 0.0", sim)
	}

	// Empty → 0.0
	if sim := cosineSim([]float64{}, []float64{}); sim != 0.0 {
		t.Errorf("cosineSim empty = %v, want 0.0", sim)
	}
}

func TestNormalizeL2(t *testing.T) {
	vec := []float64{3.0, 4.0}
	result := normalizeL2(vec)
	// |v| = 5, normalized = [0.6, 0.8]
	if result[0] != 0.6 || result[1] != 0.8 {
		t.Errorf("normalizeL2 [3,4] = %v, want [0.6, 0.8]", result)
	}

	// Zero vector → unchanged
	vec = []float64{0.0, 0.0}
	result = normalizeL2(vec)
	if result[0] != 0.0 || result[1] != 0.0 {
		t.Errorf("normalizeL2 [0,0] = %v, want [0,0]", result)
	}
}

func TestIsStopWord(t *testing.T) {
	if !isStopWord("the") {
		t.Error("'the' should be a stop word")
	}
	if !isStopWord("is") {
		t.Error("'is' should be a stop word")
	}
	if isStopWord("function") {
		t.Error("'function' should not be a stop word")
	}
	if isStopWord("python") {
		t.Error("'python' should not be a stop word")
	}
}
